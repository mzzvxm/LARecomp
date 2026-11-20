// MCLA GPU renderer -- the G-buffer's core: the sidecar images, the frame window, the debug views. See gbuffer.h.

#include "gbuffer.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "records.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gbuffer]";
constexpr uint32_t kWidthHeightRegister = 62;  // c62.xy: the anchor's frame size in guest pixels (temporal_camera.h)

// Component `comp` of register `reg` of a bank of raw big-endian bytes.
float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  const uint8_t* p = bank + size_t(reg) * 16 + size_t(comp) * 4;
  const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  return std::bit_cast<float>(bits);
}
constexpr uint32_t kIntervalFrames = 600;  // the scene's own logging interval
constexpr uint32_t kWindowLogs = 6;        // the first frames' window lines
constexpr uint32_t kTopShaders = 6;

// The guest thread's cutscene camera hook counts here; the render thread reads it once a frame (gbuffer_rules.h CutsceneGate).
std::atomic<uint64_t> g_cutscene_ticks{0};

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

const char* NameOfPlane(gbuffer::Plane plane) {
  switch (plane) {
    case gbuffer::kPlaneNormal: return "gbuffer normal";
    case gbuffer::kPlaneAlbedo: return "gbuffer albedo";
    case gbuffer::kPlaneMaterial: return "gbuffer material";
    case gbuffer::kPlaneCount: break;
  }
  return "gbuffer";
}

// "normal, albedo" of a plane mask.
std::string PlaneNames(uint32_t planes) {
  std::string text;
  const char* names[gbuffer::kPlaneCount] = {"normal", "albedo", "material"};
  for (uint32_t p = 0; p < gbuffer::kPlaneCount; ++p) {
    if ((planes & (1u << p)) == 0) continue;
    if (!text.empty()) text += ", ";
    text += names[p];
  }
  return text.empty() ? std::string("none") : text;
}

// "ABCD...=n, ..." of the most used identities of a map, most first.
std::string TopOf(const std::unordered_map<uint64_t, uint32_t>& counts) {
  std::vector<std::pair<uint64_t, uint32_t>> items(counts.begin(), counts.end());
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  std::string text;
  for (size_t i = 0; i < items.size() && i < kTopShaders; ++i) {
    if (!text.empty()) text += ", ";
    text += std::format("{:016X} x{}", items[i].first, items[i].second);
  }
  return text.empty() ? std::string("none") : text;
}

}  // namespace

GBuffer::~GBuffer() = default;

plume::RenderFormat GBuffer::FormatOf(gbuffer::Plane plane) {
  switch (gbuffer::kPlaneFormats[plane]) {
    case gbuffer::PlaneFormat::kRg16Unorm: return plume::RenderFormat::R16G16_UNORM;
    case gbuffer::PlaneFormat::kRgba8Unorm: return plume::RenderFormat::R8G8B8A8_UNORM;
    case gbuffer::PlaneFormat::kRg8Unorm: return plume::RenderFormat::R8G8_UNORM;
  }
  return plume::RenderFormat::UNKNOWN;
}

void GBuffer::NoteCutsceneCamera() {
  g_cutscene_ticks.fetch_add(1, std::memory_order_relaxed);
}

void GBuffer::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
}

void GBuffer::Shutdown() {
  // The GPU is idle and the FrameRing drained: everything goes at once, and the heap goes later so the slot is not handed back (CarRtScene::Shutdown,
  // UpscalerMasks::Shutdown). ReleaseImages() here retired instead: the EDRAM framebuffers that carry the planes (each with a view of the main depth
  // that reads the depth texture when it dies, on Vulkan) then waited in the FrameRing until the renderer's last drain, which runs after
  // EdramTargets::Shutdown freed that texture: ACCESS_VIOLATION in ~VulkanTextureView at quit (Vulkan, G-buffer on, 2026-11-20). The images' own
  // objects go in the order that they name each other: the framebuffers, then each image's framebuffer, view and texture.
  view_pipelines_.clear();
  view_shaders_ = {};
  for (HostImage& image : images_) {
    if (edram_ && image.texture) {
      edram_->DestroyFramebuffersWithExtra(image.texture.get());
    }
  }
  for (HostImage& image : images_) {
    image.framebuffer.reset();
    image.view.reset();
    image.texture.reset();
    image = HostImage{};
  }
  image_planes_ = 0;
  held_bytes_ = 0;
  frame_ = gbuffer::GBufferFrame{};
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

uint64_t GBuffer::HeldBytes() const {
  return image_planes_ != 0 ? held_bytes_ : 0;
}

void GBuffer::EndOfFrameView() {
  if (tally_.pause != 0) {
    ++pause_frames_;
    pause_draws_ += tally_.pause;
    if (tally_.mixed()) {
      ++mixed_frames_;
      if (!mixed_logged_) {
        mixed_logged_ = true;
        REXLOG_WARN("{} a frame had draws of the pause world (4 guest samples) and of the play view: {} and {} draws (the pause world must carry no sidecar; counted, logged once)", kPrefix,
                    tally_.pause, tally_.play);
      }
    }
    if (pause_spell_frames_ == 0) {
      REXLOG_INFO("{} the pause / map / garage world (4 guest samples, whatever the host's MSAA level) is on: its draws bind the guest's own framebuffer, nothing carries the sidecar", kPrefix);
      pause_spell_draws_ = 0;
    }
    ++pause_spell_frames_;
    pause_spell_draws_ += tally_.pause;
  } else if (tally_.play != 0 && pause_spell_frames_ != 0) {
    REXLOG_INFO("{} the pause / map / garage world is over: a spell of {} frames, {} draws, none carried the sidecar; frames with draws of both kinds so far: {}", kPrefix,
                pause_spell_frames_, pause_spell_draws_, mixed_frames_);
    pause_spell_frames_ = 0;
  }
  tally_ = gbuffer::ViewTally{};
}

void GBuffer::BeginFrame(uint64_t frame_serial) {
  EndOfFrameView();
  camera_.BeginFrame(frame_serial);
  frame_serial_ = frame_serial;
  frame_ = gbuffer::GBufferFrame{};
  window_.BeginFrame();
  const gbuffer::Setting setting = gbuffer::SettingOfWord(larecomp::settings::mcla_gpu_gbuffer.Get());
  plan_ = gbuffer::PlanFrame(setting, wants_.Union());
  view_ = gbuffer::ViewOf(larecomp::settings::mcla_gpu_gbuffer_view.Get());
  caps_ = gbuffer::CapsFor(ActiveBackend() == Backend::kVulkan);
  masked_attach_ = gbuffer::MaskedDrawsAttach(gbuffer::AttachOfWord(larecomp::settings::mcla_gpu_gbuffer_attach.Get()), caps_);
  suppressed_ = cutscene_gate_.Update(g_cutscene_ticks.load(std::memory_order_relaxed));
  if (plan_.attached != last_plan_.attached || plan_.written != last_plan_.written) {
    if (plan_.attached != 0) {
      REXLOG_INFO("{} the G-buffer is on (mcla_gpu_gbuffer; live): asked for {} (the setting, or the effects: ambient occlusion {}, bounce light {}, local lights {}, "
                  "reflections {}, decals {}), written: {}, the draws that carry it have {} plane{}",
                  kPrefix, PlaneNames(plan_.requested), PlaneNames(wants_.Of(gbuffer::Consumer::kAmbientOcclusion)), PlaneNames(wants_.Of(gbuffer::Consumer::kBounceLight)),
                  PlaneNames(wants_.Of(gbuffer::Consumer::kLocalLights)), PlaneNames(wants_.Of(gbuffer::Consumer::kReflections)), PlaneNames(wants_.Of(gbuffer::Consumer::kDecals)),
                  PlaneNames(plan_.written), gbuffer::PlaneCount(plan_.attached), gbuffer::PlaneCount(plan_.attached) == 1 ? "" : "s");
    } else {
      REXLOG_INFO("{} the G-buffer is off (mcla_gpu_gbuffer {}; no effect asks for a plane the stage writes)", kPrefix, larecomp::settings::mcla_gpu_gbuffer.Get());
    }
    blocked_ = false;  // asked again at the next clear
    blocked_logged_ = false;
    if (plan_.attached == 0 && image_planes_ != 0) {
      ReleaseImages();
    }
  }
  last_plan_ = plan_;
  if (plan_.attached == 0) {
    return;
  }
  if (!masked_attach_logged_) {
    masked_attach_logged_ = true;
    REXLOG_INFO("{} the draws that do not write the planes {} (mcla_gpu_gbuffer_attach {}; this back end has a blend state per colour target: {})", kPrefix,
                masked_attach_ ? "bind them too: the opaque ones clear what they cover, the blended ones leave it" : "do not bind them: the planes keep what an earlier draw wrote there",
                larecomp::settings::mcla_gpu_gbuffer_attach.Get(), caps_.per_target_blend ? "yes" : "no");
  }
  if (suppressed_) {
    ++suppressed_frames_;  // a cutscene's frame: nothing carries the sidecar (active() says no), and it is not one of the frames the counts are per
    return;
  }
  ++active_frames_;
  if (++interval_frames_ >= kIntervalFrames) {
    LogInterval();
  }
}

void GBuffer::OnAnchor(const uint8_t* ps_bank) {
  ++anchors_;
  // The frame's camera for the NEXT frame's draws. Anything that is not the play view's (the pause world, a frame with no 3D view, both) keeps none (CameraKeeper::OnAnchor).
  gbuffer::CameraAxes axes;
  bool have = false;
  const float width = BankFloat(ps_bank, kWidthHeightRegister, 0);
  const float height = BankFloat(ps_bank, kWidthHeightRegister, 1);
  if (width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f) {
    temporal::Camera camera;
    if (temporal::ExtractCamera(ps_bank, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_serial_, &camera) == temporal::Reject::kNone && camera.fov_y > 0.0 && camera.aspect > 0.0) {
      const double tan_y = std::tan(camera.fov_y * 0.5);
      const auto axis = [](const double v[3]) { return gbuffer::Vec3{float(v[0]), float(v[1]), float(v[2])}; };
      axes.right = axis(camera.right);
      axes.up = axis(camera.up);
      axes.forward = axis(camera.forward);
      axes.tan_half_y = float(tan_y);
      axes.tan_half_x = float(tan_y * camera.aspect);
      axes.width = camera.width;
      axes.height = camera.height;
      have = camera.width != 0 && camera.height != 0;
    }
  }
  camera_.OnAnchor(have ? &axes : nullptr, tally_);
  if (have && tally_.play_world()) {
    ++anchors_kept_;
  }
  if (!camera_logged_ && anchors_ >= 3) {
    camera_logged_ = true;
    REXLOG_INFO("{} the camera rows of the derivative normal come from the game's motion blur draw (the anchor), read here whether or not an upscaler runs: {} of the first {} anchors gave a "
                "play view camera",
                kPrefix, anchors_kept_, anchors_);
  }
}

void GBuffer::NoteDraw(gbuffer::Why why, uint64_t ps_identity) {
  ++counts_[size_t(why)];
  tally_.Note(why);
  if (why == gbuffer::Why::kWrites) {
    ++written_by_[ps_identity];
  } else if (why == gbuffer::Why::kMaskedNoVariant || why == gbuffer::Why::kMaskedOtherVariant) {
    ++masked_by_[ps_identity];
  }
}

void GBuffer::Transition(plume::RenderCommandList* list, plume::RenderTextureLayout layout) {
  plume::RenderTextureBarrier barriers[gbuffer::kPlaneCount];
  uint32_t count = 0;
  for (HostImage& image : images_) {
    if (!image.texture || image.layout == layout) {
      continue;
    }
    barriers[count++] = plume::RenderTextureBarrier(image.texture.get(), layout);
    image.layout = layout;
  }
  if (count != 0) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, count);
  }
}

bool GBuffer::CheckAffordable(const EdramTargets::Target& colour0) {
  const uint32_t width = colour0.scale.Edge(colour0.width);
  const uint32_t height = colour0.scale.Edge(colour0.height);
  const uint32_t samples = gbuffer::SidecarSamples(colour0.host_samples);
  const bool fits = gbuffer::SidecarAffordable(plan_.attached, width, height, samples);
  blocked_ = !fits;
  if (!fits) {
    if (!blocked_logged_) {
      blocked_logged_ = true;
      REXLOG_WARN("{} no sidecar: {}x{} at {} sample{} would be {:.0f} MiB (the cap is {:.0f} MiB): the consumers use the depth (retried at each clear of the main colour)", kPrefix,
                  width, height, samples, samples == 1 ? "" : "s", Mib(gbuffer::SidecarBytes(plan_.attached, width, height, samples)), Mib(gbuffer::kMaxSidecarBytes));
    }
    if (image_planes_ != 0) {
      ReleaseImages();
    }
  } else {
    blocked_logged_ = false;
  }
  return fits;
}

bool GBuffer::EnsureImages(uint32_t width, uint32_t height, uint32_t samples) {
  samples = gbuffer::SidecarSamples(samples);
  const uint32_t want = plan_.attached;
  bool same = image_planes_ == want && want != 0;
  for (uint32_t p = 0; p < gbuffer::kPlaneCount && same; ++p) {
    if ((want & (1u << p)) != 0) {
      same = images_[p].texture && images_[p].width == width && images_[p].height == height && images_[p].samples == samples;
    }
  }
  if (same) {
    return true;
  }
  if (want == 0) {
    return false;
  }
  // The framebuffers that hold the old textures go first (after the lists that still use them); the images' own release is HostPasses'. All the planes are made
  // again together: a framebuffer cannot mix sizes or samples.
  ReleaseImages();
  for (uint32_t p = 0; p < gbuffer::kPlaneCount; ++p) {
    if ((want & (1u << p)) == 0) {
      continue;
    }
    if (!passes_->EnsureImage(images_[p], width, height, FormatOf(gbuffer::Plane(p)), NameOfPlane(gbuffer::Plane(p)), plume::RenderTextureFlag::RENDER_TARGET, samples)) {
      ReleaseImages();
      return false;
    }
  }
  image_planes_ = want;
  ++generation_;
  needs_clear_ = true;
  held_bytes_ = gbuffer::SidecarBytes(want, width, height, samples);
  // The shape of the spike's line for the normal plane alone (perf/scripts/gbuffer_spike_report.py reads it), a list of the planes for more.
  const std::string what = want == gbuffer::PlaneBit(gbuffer::kPlaneNormal)
                               ? std::format("the normal plane {}x{} R16G16_UNORM", width, height)
                               : std::format("planes {} {}x{}", PlaneNames(want), width, height);
  REXLOG_INFO("{} sidecar: {}, {} sample{} ({:.1f} MiB), generation {}", kPrefix, what, samples, samples == 1 ? "" : "s", Mib(held_bytes_), generation_);
  return true;
}

void GBuffer::ReleaseImages() {
  for (HostImage& image : images_) {
    if (image.texture) {
      edram_->RetireFramebuffersWithExtra(image.texture.get());
    }
    passes_->ReleaseImage(image);
  }
  image_planes_ = 0;
  held_bytes_ = 0;
  frame_ = gbuffer::GBufferFrame{};
}

void GBuffer::ClearRect(plume::RenderCommandList* list, const plume::RenderRect& host) {
  const plume::RenderTexture* extras[gbuffer::kPlaneCount] = {};
  uint32_t count = 0;
  for (const HostImage& image : images_) {
    if (image.texture) {
      extras[count++] = image.texture.get();
    }
  }
  // One framebuffer of all the planes (the colour slots after the guest's are the planes' slots: the Framebuffer's key holds the textures and the generation).
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(nullptr, 0, nullptr, extras, count, generation_);
  if (!framebuffer) {
    return;
  }
  Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
  list->setFramebuffer(framebuffer);
  for (uint32_t i = 0; i < count; ++i) {
    list->clearColor(i, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f), &host, 1);
  }
  framebuffer_dirty_ = true;
}

void GBuffer::FillSidecar(Sidecar* out) const {
  *out = Sidecar{};
  for (const HostImage& image : images_) {
    if (image.texture) {
      out->textures[out->count++] = image.texture.get();
    }
  }
  out->generation = generation_;
}

bool GBuffer::Attach(plume::RenderCommandList* list, EdramTargets::Target& colour0, Sidecar* out) {
  if (!CheckAffordable(colour0) || !EnsureImages(colour0.scale.Edge(colour0.width), colour0.scale.Edge(colour0.height), colour0.host_samples)) {
    return false;
  }
  const bool no_clear_yet = window_.GateForDraw() == gbuffer::Window::Gate::kNeedsClear;
  if (needs_clear_ || no_clear_yet) {
    // A draw of the frame came before the guest's clear of the main colour (or the images are new): the whole of them is made "no data" first.
    if (list) {
      ClearRect(list, plume::RenderRect(0, 0, int32_t(images_[gbuffer::kPlaneNormal].width), int32_t(images_[gbuffer::kPlaneNormal].height)));
    }
    needs_clear_ = false;
    if (no_clear_yet) {
      window_.OnInitialClear();
      ++initial_clears_;
      if (window_logs_ < kWindowLogs) {
        ++window_logs_;
        REXLOG_INFO("{} frame {}: a main view draw came before any clear of the main colour: the sidecar was cleared whole first", kPrefix, frame_serial_);
      }
    }
  }
  Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
  FillSidecar(out);
  return true;
}

void GBuffer::OnColorClear(plume::RenderCommandList* list, EdramTargets::Target& colour0, const plume::RenderRect& guest_rect) {
  if (!tracking() || !list) {
    return;
  }
  if (window_.triggered()) {
    ++late_clears_;  // after the frame was published: the images are the consumers', the clear belongs to something else
    return;
  }
  if (!CheckAffordable(colour0) || !EnsureImages(colour0.scale.Edge(colour0.width), colour0.scale.Edge(colour0.height), colour0.host_samples)) {
    return;
  }
  plume::RenderRect rect = guest_rect;
  rect.right = std::min(rect.right, int32_t(colour0.width));
  rect.bottom = std::min(rect.bottom, int32_t(colour0.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  const bool whole = rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(colour0.width) &&
                     rect.bottom + int32_t(EdramTargets::kHeightAlignment) >= int32_t(colour0.height);
  // New images have undefined contents: whatever the rectangle, the whole of them is cleared now.
  const HostImage& first = images_[gbuffer::kPlaneNormal];
  const plume::RenderRect host = needs_clear_ ? plume::RenderRect(0, 0, int32_t(first.width), int32_t(first.height))
                                              : EdramTargets::ScaledRect(rect, colour0.scale);
  ClearRect(list, host);
  needs_clear_ = false;
  window_.OnClear(whole);
  ++clears_;
  if (!whole) ++partial_clears_;
  if (window_logs_ < kWindowLogs) {
    ++window_logs_;
    REXLOG_INFO("{} frame {}: the guest cleared the main colour ({},{})-({},{}) of {}x{}: {} clear of the sidecar, clear {} of the frame", kPrefix, frame_serial_,
                rect.left, rect.top, rect.right, rect.bottom, colour0.width, colour0.height, whole ? "whole" : "partial", window_.clears());
  }
}

bool GBuffer::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (!active() || !list) {
    return false;
  }
  gbuffer::TriggerFacts facts;
  facts.depth_copy = summary.depth_copy;
  facts.main_depth_samples = summary.depth_copy ? temporal::MainDepthSamples(summary.source_key) : 0u;
  facts.dest_address = summary.dest_address;
  facts.clears_depth = summary.clears_depth;
  facts.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
  facts.first_this_frame = !window_.triggered();
  if (!gbuffer::IsTrigger(facts)) {
    return false;
  }
  window_.OnTrigger();
  if (image_planes_ == 0 || !window_.had_clear()) {
    return false;  // the frame carried no sidecar (a view with nothing of the family, or the images could not be made)
  }
  Transition(list, plume::RenderTextureLayout::SHADER_READ);
  frame_ = gbuffer::GBufferFrame{};
  frame_.generation = generation_;
  frame_.frame_serial = uint32_t(frame_serial_);
  frame_.width = images_[gbuffer::kPlaneNormal].width;
  frame_.height = images_[gbuffer::kPlaneNormal].height;
  frame_.host_samples = images_[gbuffer::kPlaneNormal].samples;
  frame_.sample0 = summary.host_sample;
  for (uint32_t p = 0; p < gbuffer::kPlaneCount; ++p) {
    if ((plan_.written & (1u << p)) != 0 && images_[p].texture) {
      frame_.valid |= 1u << p;
      frame_.slot[p] = images_[p].slot;
    }
  }
  ++published_frames_;
  return true;
}

plume::RenderPipeline* GBuffer::ViewPipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (ViewPipeline& v : view_pipelines_) {
    if (v.format == format && v.samples == samples) {
      return v.pipeline.get();
    }
  }
  if (view_shaders_failed_) {
    return nullptr;
  }
  const size_t slot = samples > 1 ? 1 : 0;
  if (!view_shaders_[slot]) {
    const HostShaderBlob blob = GBufferViewPixelShader(samples > 1);
    view_shaders_[slot] = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (!view_shaders_[slot]) {
      view_shaders_failed_ = true;
      REXLOG_ERROR("{} the debug view's shader could not be made: no debug view (reported once)", kPrefix);
      return nullptr;
    }
  }
  ViewPipeline v;
  v.format = format;
  v.samples = samples;
  v.pipeline = passes_->CreatePipeline(passes_->layout(), *view_shaders_[slot], format, plume::RenderBlendDesc::Copy(), samples, "gbuffer view");
  if (!v.pipeline) {
    view_shaders_failed_ = true;
    REXLOG_ERROR("{} the debug view's pipeline could not be made: no debug view (reported once)", kPrefix);
    return nullptr;
  }
  plume::RenderPipeline* raw = v.pipeline.get();
  view_pipelines_.push_back(std::move(v));
  return raw;
}

bool GBuffer::DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (view_ == gbuffer::View::kOff || !list || !summary.source || !frame_.Has(gbuffer::kPlaneNormal)) {
    return false;
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != frame_.host_samples || found->scale.Edge(found->width) != frame_.width ||
      found->scale.Edge(found->height) != frame_.height) {
    return false;
  }
  plume::RenderPipeline* pipeline = ViewPipelineFor(found->format, found->host_samples);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  ViewPush push;
  push.slots[0] = frame_.slot[gbuffer::kPlaneNormal];
  push.slots[1] = frame_.slot[gbuffer::kPlaneAlbedo];
  push.slots[2] = frame_.slot[gbuffer::kPlaneMaterial];
  push.slots[3] = uint32_t(view_);
  push.info[0] = frame_.sample0;
  push.info[1] = frame_.valid;
  push.info[2] = summary.sample_slot;  // the main depth's plane (the missing pixels view)
  pass_timing_->Mark(list, "gbuffer view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(frame_.width), float(frame_.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(frame_.width), int32_t(frame_.height));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

void GBuffer::LogInterval() {
  const double frames = double(std::max<uint64_t>(active_frames_, 1));
  const auto n = [&](gbuffer::Why why) { return double(counts_[size_t(why)]) / frames; };
  using gbuffer::Why;
  // The first two lines keep the spike's shapes (perf/scripts/gbuffer_spike_report.py reads them); what the core added is in the third.
  REXLOG_INFO("{} {} frames with the G-buffer on, {} published: draws a frame: writes {:.1f}, masked {:.1f} (blended {:.1f}, no variant {:.1f}, other variant {:.1f}, no colour {:.1f}), "
              "not carried: not the main view {:.1f}, pause view {:.1f}, window closed {:.1f}; clears {} ({} partial, {} before any clear, {} after the trigger); "
              "sidecar {:.1f} MiB",
              kPrefix, active_frames_, published_frames_, n(Why::kWrites),
              n(Why::kMaskedBlended) + n(Why::kMaskedNoVariant) + n(Why::kMaskedOtherVariant) + n(Why::kMaskedNoColor), n(Why::kMaskedBlended), n(Why::kMaskedNoVariant),
              n(Why::kMaskedOtherVariant), n(Why::kMaskedNoColor), n(Why::kNotMainView), n(Why::kPauseView), n(Why::kWindowClosed), clears_, partial_clears_, initial_clears_, late_clears_,
              Mib(HeldBytes()));
  REXLOG_INFO("{} pixel shaders that wrote it: {}; carried it with no variant (the next families): {}", kPrefix, TopOf(written_by_), TopOf(masked_by_));
  REXLOG_INFO("{} also a frame: draws in the window that cover the main colour and write no plane: no depth target {:.1f}, another depth {:.1f}, a window of the target (the mirror) {:.1f}; "
              "writers cut by RT0's write mask {:.1f}; without the sidecar for want of its pipeline {:.1f}; frames without it: cutscenes {}; attach: {}",
              kPrefix, n(Why::kMaskedNoDepth), n(Why::kMaskedOtherDepth), n(Why::kMaskedSubView), n(Why::kMaskedNarrowMask), double(fallback_draws_) / frames, suppressed_frames_,
              masked_attach_ ? "all draws (the non writers clear or mask)" : "writers only");
  // The pause world (guest 4x) and the camera of the derivative normal: the pause world never carries, so its draws are in "pause view" above and its frames here; a writer's constants
  // have the camera rows when the last frame's anchor gave a play view camera.
  REXLOG_INFO("{} views and camera: pause world frames {} ({} draws), frames with draws of both kinds {} (must be 0); anchors {} of which {} gave a play view camera; writers' constants with "
              "the camera rows {}, without {}",
              kPrefix, pause_frames_, pause_draws_, mixed_frames_, anchors_, anchors_kept_, camera_rows_, camera_missing_);
  pause_frames_ = 0;
  pause_draws_ = 0;
  mixed_frames_ = 0;
  anchors_ = 0;
  anchors_kept_ = 0;
  camera_rows_ = 0;
  camera_missing_ = 0;
  interval_frames_ = 0;
  active_frames_ = 0;
  published_frames_ = 0;
  suppressed_frames_ = 0;
  fallback_draws_ = 0;
  counts_.fill(0);
  clears_ = 0;
  partial_clears_ = 0;
  initial_clears_ = 0;
  late_clears_ = 0;
  written_by_.clear();
  masked_by_.clear();
}

}  // namespace mcla::gpu
