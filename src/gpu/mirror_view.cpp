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
#include "mirror_setting.h"
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
// The main colour's host format (k_2_10_10_10_FLOAT as a 16 bit float texture, car_mirror_rt.cpp's kApplyFormat too): the pipelines listed ahead are made for it; another format is made when first used.
constexpr plume::RenderFormat kMainFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
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

// Guest render thread. The hook has selected the phase. Keep its marker even if the setting changes while the phase is in flight.
void NoteMirrorPhase(bool begin, bool hud_stamp) {
  static std::atomic<bool> g_open{false};  // trivially destructible: nothing to leak at the hard exit
  if (begin) {
    g_open.store(true, std::memory_order_relaxed);
    // The phase's own field goes with its begin (MV-J2): the camera of this phase was built just before, on this thread, so the latest camera facts are its (mirror_view_link.h PhaseField).
    OnGuestSceneComposited(hud_stamp ? mirror_view::kMarkerBegin : mirror_view::kMarkerBeginNoStamp,
                          mirror_view::PackPhaseField(mirror_view::PhaseFieldOf(mirror_view::CurrentCamera())));
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
  base_listed_ = false;
  // The two pipelines (the average of the view's samples, the HUD stamp) are the host precache's: listed here where the switch is on at the start, so that the render thread never compiles one between two
  // frames; with the switch off nothing is made until it is first wanted (the first redirected phase lists them for its formats, made in the background: Use).
  const int32_t mode = mirror_setting::Current().mode;  // the SIDE MIRRORS row (or the debug force) as it stands at the start
  if (mode >= 1 && mode <= 4 && MakeShaders()) {
    ListBase();
  } else {
    passes.precache().Note("mirror view: off at the start (the SIDE MIRRORS row and mcla_gpu_mirror_view): its pipelines are made in the background when it is first wanted", 0);
  }
}

void MirrorView::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (CarShadowMap::Shutdown).
  colour_ = HostImage{};
  resolved_ = HostImage{};
  framebuffer_.reset();
  depth_.reset();
  // The host builder's functions name this object's shaders: stop it first (CarShadowMap::Shutdown).
  if (passes_) {
    passes_->precache().Stop();
  }
  pipelines_.clear();
  listed_.clear();
  failed_.clear();
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

void MirrorView::BeginFrame(bool rt_side_mirrors) {
  ++frame_;
  const mirror_setting::Result setting = mirror_setting::Current();  // the SIDE MIRRORS row and the debug switches under it, once a frame (mirror_setting.h); the guest reads the same answer through MirrorViewMode()
  mirror_setting::LogIfChanged(setting);
  mode_ = setting.mode;
  if (mode_ < 0 || mode_ > 4) {
    mode_ = 0;
  }
  {
    // The host's request to the guest (mirror_view_link.h Request), once a frame, here and nowhere else: with the switch off it says "no redirect" (the guest half does what the game would do alone), and
    // the side mirrors' need is MV3's flag (mirror_glass_need.h) unless the ray traced side mirrors win, which the guest reads only through this request, with the wider field they want (JOIN2:
    // mirror_view_rules.h RequestFor, SideMirrorsField). MirrorView::Shutdown withdraws it.
    mirror_view::RequestFacts request;
    request.enabled = enabled();
    request.blocked = Blocked();
    request.side_need = mirror_glass::SideNeed();
    request.rt_side_mirrors = rt_side_mirrors;
    request.hud_stamp = mirror_view::StampWanted();
    request.cls = SizeClass();
    request.retail_hfov_deg = double(mirror_view::CurrentCamera().retail_hfov_deg);
    const mirror_draws::Rect hud = mirror_draws::Current();
    request.stamp_aspect = hud.valid() ? double(hud.w) / double(hud.h) : 0.0;
    request.every = setting.every;  // the row's level: HIGH redraws every 2nd frame, ULTRA every frame (1 when the debug switch forces the view)
    mirror_view::PublishRequest(mirror_view::RequestFor(request));
  }
  phase_.FrameStart();  // a phase never outlives its frame: a lost end marker closes here (counted as unbalanced)
  used_ = false;
  failed_phase_ = false;
  stamp_armed_ = false;
  overlay_armed_ = false;
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

bool MirrorView::BeginPhase(const mirror_draws::Rect& rect, const mirror_view::PhaseField& field, bool hud_stamp) {
  mirror_view::BeginFacts facts;
  facts.enabled = enabled();
  facts.rect_valid = rect.valid();
  facts.hud_stamp = hud_stamp;
  phase_hud_stamp_ = hud_stamp;
  // The size follows the field the camera of THIS phase was built with (the marker's tag): the requested side mirrors' field where the guest applied it, else the rectangle's aspect at the game's own field
  // (PlanForPhase).
  phase_field_ = field;
  plan_ = mirror_view::PlanForPhase(SizeClass(), double(rect.w), double(rect.h), field);
  if (!(field == logged_field_)) {
    // One line at every change of the phase's own field (a mirror coming into view, the HUD stamp coming and going): it must follow the guest's "[RearView] camera" line by one to three frames (CHECK J2).
    logged_field_ = field;
    REXLOG_INFO("{} phase field: {}", kPrefix,
                field.known() ? std::format("{:.2f} x {:.2f} degrees (across, up), {}", double(field.hfov_deg), double(field.vfov_deg), field.requested ? "the host's request: the view is sized by it" : "the game's own: the view is the rectangle's")
                              : std::string("none (no camera built yet)"));
  }
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
    ListLive(color, samples);
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
    return false;  // the phase did not use the view
  }
  ++stats_.completed;
  if (!was_used || failed) {
    // A phase with no draw and no clear (the guest's held frame sends the two markers back to back) or one whose images failed: the picture of the phase before stays, and the frame's stamp is drawn
    // from it (StampDue holds the age).
    stamp_armed_ = whole_ && !failed && mirror_view::StampDue(whole_, uint32_t(std::min<uint64_t>(frame_ - picture_frame_, 0xFFFFFFFFu)), rect_.valid(), phase_hud_stamp_);
    overlay_armed_ = whole_ && !failed && frame_ - picture_frame_ <= mirror_view::kMaxHeldFrames;
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
    bool pending = false;
    plume::RenderPipeline* pipeline = ResolvePipeline(colour_.format, &pending);
    if (pipeline && resolved_.texture) {
      const ResolvePush push = {{colour_.slot, samples_, 0, 0}};
      pass_timing_->Mark(list, "mirror view resolve");
      passes_->DrawInto(list, resolved_, pipeline, &push, sizeof(push));
    } else {
      // No resolve: the readers have no picture this frame (no stamp until a phase's resolve is made).
      made = false;
      if (!pending) {
        REXLOG_ERROR("{} the resolve pipeline for format {} could not be made: no picture this frame", kPrefix, uint32_t(colour_.format));
      }
    }
  }
  pass_timing_->Mark(list, "after mirror view");
  if (made) {
    whole_ = true;
    picture_plan_ = plan_;
    picture_field_ = phase_field_;
    picture_frame_ = frame_;
    stamp_armed_ = mirror_view::StampDue(whole_, 0, rect_.valid(), phase_hud_stamp_);
    overlay_armed_ = true;
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
  bool pending = false;
  plume::RenderPipeline* pipeline = StampPipeline(colour.format, colour.host_samples, &pending);
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
  // The whole picture over the whole rectangle where the view is the HUD's own (the rectangle's aspect at the game's own field: PlanForRect); where the side mirrors made it wider (a plan with a field of
  // its own) the centred part that has the HUD's retail field across and the rectangle's aspect (mirror_view::HudCrop): the HUD mirror shows what it always did.
  const double stamp_aspect = rect_.valid() ? double(rect_.w) / double(rect_.h) : 0.0;
  const mirror_view::Crop crop = mirror_view::HudCrop(picture_plan_, double(mirror_view::CurrentCamera().retail_hfov_deg), stamp_aspect);
  const StampPush push = {{slot, 0, 0, 0},
                          {float(host.left), float(host.top), float(host.right - host.left), float(host.bottom - host.top)},
                          {float(crop.u0), float(crop.v0), float(crop.u1), float(crop.v1)}};
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

// The variants of the host precache's list: the colour format in the high bits, bit 7 for the stamp, the sample count's index (1, 2, 4, 8 -> 0 to 3) for the stamp.
uint32_t MirrorView::ResolveVariant(plume::RenderFormat format) { return uint32_t(format) << 8; }
uint32_t MirrorView::StampVariant(plume::RenderFormat format, uint32_t host_samples) {
  const uint32_t index = host_samples == 1 ? 0u : host_samples == 2 ? 1u : host_samples == 4 ? 2u : 3u;
  return (uint32_t(format) << 8) | 0x80u | index;
}

namespace {
uint64_t PrecacheKey(uint32_t variant) { return HostPrecache::Key(HostPrecache::kMirrorView, variant); }
std::string VariantName(uint32_t variant) {
  if ((variant & 0x80u) == 0) {
    return std::format("mirror view resolve (format {})", variant >> 8);
  }
  return std::format("mirror view stamp (format {}, {} samples)", variant >> 8, 1u << (variant & 3u));
}
}  // namespace

std::unique_ptr<plume::RenderPipeline> MirrorView::MakeVariant(uint32_t variant) {
  if (!passes_ || !resolve_shader_ || !stamp_shader_) {
    return nullptr;
  }
  const plume::RenderFormat format = plume::RenderFormat(variant >> 8);
  const std::string name = VariantName(variant);
  if ((variant & 0x80u) == 0) {
    return passes_->CreatePipeline(*resolve_shader_, format, name.c_str());
  }
  // The picture replaces the rectangle's colour, alpha included (the game's own mirror wrote the alpha of its draws there), no blend and no depth.
  return passes_->CreatePipeline(passes_->layout(), *stamp_shader_, format, plume::RenderBlendDesc::Copy(), 1u << (variant & 3u), name.c_str());
}

// At the start, where the switch is on: the main format's average and its stamp for the sample counts the main colour has in play (1 under an upscaler or with MSAA off, 2 the game's, 4 by the MSAA level; 8 is
// made in the background when first wanted).
void MirrorView::ListBase() {
  base_listed_ = true;
  HostPrecache& precache = passes_->precache();
  uint32_t added = 0;
  const auto add = [&](uint32_t variant) {
    if (listed_.insert(variant).second) {
      added += precache.Add(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakeVariant(variant); }) ? 1u : 0u;
    }
  };
  add(ResolveVariant(kMainFormat));
  for (uint32_t samples : {1u, 2u, 4u}) {
    add(StampVariant(kMainFormat, samples));
  }
  precache.Note("mirror view: on at the start (mcla_gpu_mirror_view): the average of the view's samples and the HUD stamp for 1, 2 and 4 host samples", added);
}

// The first redirected phase of a session (or of a format the start did not list): the pipelines for its format and samples go to the background builder together (one worker), and the readers draw from
// the frame they are made.
void MirrorView::ListLive(plume::RenderFormat color, uint32_t samples) {
  std::vector<HostPrecache::Item> fresh;
  const auto add = [&](uint32_t variant) {
    if (listed_.insert(variant).second) {
      fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakeVariant(variant); }});
    }
  };
  add(ResolveVariant(color));
  add(StampVariant(color, samples));
  if (!fresh.empty()) {
    const uint32_t count = passes_->precache().AddLive("mirror view", std::move(fresh));
    REXLOG_INFO("{} {} pipelines are made in the background (the view was off at the start, or its format is new): the readers draw from the frame they are done", kPrefix, count);
  }
}

plume::RenderPipeline* MirrorView::Pipeline(uint32_t variant, bool* pending) {
  const auto found = pipelines_.find(variant);
  if (found != pipelines_.end()) {
    return found->second.get();
  }
  if (failed_.count(variant) != 0 || !MakeShaders()) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (listed_.insert(variant).second) {
    // Nobody listed it (ListLive does for the phase's own format): the background builder makes it; the readers have none until it is made.
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakeVariant(variant); }});
    precache.AddLive("mirror view", std::move(fresh));
    *pending = true;
    return nullptr;
  }
  if (!precache.Ready(PrecacheKey(variant))) {
    *pending = true;
    return nullptr;
  }
  std::unique_ptr<plume::RenderPipeline> made = precache.Acquire(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakeVariant(variant); });
  if (!made) {
    failed_.insert(variant);
    return nullptr;
  }
  return (pipelines_[variant] = std::move(made)).get();
}

plume::RenderPipeline* MirrorView::ResolvePipeline(plume::RenderFormat format, bool* pending) { return Pipeline(ResolveVariant(format), pending); }

plume::RenderPipeline* MirrorView::StampPipeline(plume::RenderFormat format, uint32_t host_samples, bool* pending) { return Pipeline(StampVariant(format, host_samples), pending); }

bool MirrorView::PictureField(float* hfov_deg, float* vfov_deg) const {
  if (!whole_ || !picture_field_.known()) {
    return false;
  }
  *hfov_deg = picture_field_.hfov_deg;
  *vfov_deg = picture_field_.vfov_deg;
  return true;
}

MirrorView::Picture MirrorView::CurrentPicture() const {
  Picture p;
  p.whole = whole_ && (samples_ > 1 ? resolved_.texture != nullptr : colour_.texture != nullptr);
  p.slot = PictureSlot();
  p.age = uint32_t(std::min<uint64_t>(frame_ - picture_frame_, 0xFFFFFFFFu));
  p.width = colour_.width;
  p.height = colour_.height;
  return p;
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
  REXLOG_INFO("{} images made: {}x{} at {} sample{} ({:.2f} MiB: colour {}, depth {}, picture {}), for the class size {}, {}", kPrefix, width, height, samples, samples == 1 ? "" : "s", Mib(HeldBytes()),
              uint32_t(color), uint32_t(depth), samples > 1 ? "averaged" : "the colour itself", mode_ == 1 ? "of the card" : "forced",
              plan_.hfov_deg > 0.0 ? std::format("the side mirrors' field {:.1f} x {:.1f} degrees", plan_.hfov_deg, plan_.vfov_deg) : std::string("the game's own field"));
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
  picture_plan_ = mirror_view::Plan{};
  picture_field_ = mirror_view::PhaseField{};
  stamp_armed_ = false;
  overlay_armed_ = false;
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
