// MCLA GPU renderer -- the guest's EDRAM render targets on the host. See
// edram.h.

#include "edram.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "gpu.h"
#include "records.h"
#include "render_scale.h"
#include "temporal_identities.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_edram_aliasing, false, "MCLA/GPU",
    "EDRAM inheritance detector (M5.2d): per EDRAM "
    "tile and aspect, the identity that wrote it last, reporting a draw or a resolve that would "
    "see another identity's bytes on the console -- which the host does not carry over. Off: "
    "none of its work is done.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_coverage_queries, false, "MCLA/GPU",
    "With mcla_gpu_edram_aliasing: an occlusion query around each opaque draw the detector "
    "takes to cover its scissor, checking that it did. Costs GPU time, for "
    "checking a report. Off: no query pool is made, read or reset, and such takes go "
    "unchecked (listed once per pair).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_edram_capture, 0, "MCLA/GPU",
    "With mcla_gpu_edram_aliasing: a RenderDoc capture of the frame after the first report of "
    "each kind picked by these bits, 4 at most: 1 a pair met by a write that keeps "
    "or blends another identity's bytes, 2 a pair met by a read, 4 the first reader of "
    "inherited bytes of each kind. 0: none. Needs the game started under RenderDoc.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_ghost_log_interval, 0, "MCLA/GPU",
    "Diagnostic: log one live frame in every N with the 1280x720 scene clears "
    "and three-stripe resolves, plus all EDRAM target creations and growths. 0: off; 1: every "
    "frame. Hot reload for reproducing the frame ghost.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_msaa, "2", "MCLA/GPU",
    "MSAA of the images of the screen the game multisamples (M6d.2): off (the game's own switch, "
    "disable_msaa), 2 (the console's), 4 or 8 (more samples on the host; the guest keeps its "
    "own). Targets the game does not multisample, or that are not images of the screen, keep "
    "the game's samples. A game started with an upscaler (mcla_gpu_upscaler) that runs on this PC "
    "starts without MSAA whatever this says; the choice is kept and applies again at a start "
    "with no upscaler.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

namespace {

// The game's screen size: grcDevice's width and height (IDB: 0x827D4274 and
// 0x827D4278, set at start; the render setup sub_822E47E0 makes the scene's
// tiling buffers that size and the post-processing sizes FrameBufferSize from
// it, sub_8260E9A0).
constexpr uint32_t kGuestScreenWidthAddress = 0x827D4274;
constexpr uint32_t kGuestScreenHeightAddress = 0x827D4278;

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t BytesPerPixel(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R16G16B16A16_FLOAT:
    case plume::RenderFormat::R16G16B16A16_SNORM:
    case plume::RenderFormat::R32G32_FLOAT:
    case plume::RenderFormat::D32_FLOAT_S8_UINT:
      return 8;
    default:
      return 4;
  }
}

}  // namespace

uint32_t FoldColorFormat(uint32_t color_format) {
  switch (color_format) {
    case 10: return 2;  // k_2_10_10_10_AS_10_10_10_10
    case 12: return 3;  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    default: return color_format;
  }
}

bool EdramTargets::ScreenSize(uint32_t* width, uint32_t* height) {
  if (screen_width_ == 0) {
    const rex::Runtime* runtime = rex::Runtime::instance();
    const uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
    if (!base) {
      return false;
    }
    const uint32_t w = LoadBe32(base + kGuestScreenWidthAddress);
    const uint32_t h = LoadBe32(base + kGuestScreenHeightAddress);
    // Static 1x1 until the game sets it at start.
    if (w < 64 || h < 64 || w > kMaxExtent || h > kMaxExtent) {
      if (!screen_unset_logged_) {
        screen_unset_logged_ = true;
        REXLOG_ERROR("[gpu] edram: the game's screen size reads {}x{}, not set yet: targets made "
                     "now get the guest's size (counted as unknown); read again on the next one",
                     w, h);
      }
      return false;
    }
    screen_width_ = w;
    screen_height_ = h;
    REXLOG_INFO("[gpu] edram: the game's screen is {}x{}; its images (and halvings down to an "
                "eighth) are made at {}x",
                w, h, resolution_scale_);
    if (scale_from_window_ && (w != kAssumedScreenWidth || h != kAssumedScreenHeight)) {
      REXLOG_WARN("[gpu] edram: the render scale {}x was measured from the window against a "
                  "{}x{} screen, and the game's is {}x{}: the render is not the size the "
                  "screen's scale or the FSR1 preset meant",
                  resolution_scale_, kAssumedScreenWidth, kAssumedScreenHeight, w, h);
    }
  }
  *width = screen_width_;
  *height = screen_height_;
  return true;
}

bool EdramTargets::IsScreenImage(uint32_t size_bits) {
  uint32_t screen_width = 0, screen_height = 0;
  if (size_bits == 0 || !ScreenSize(&screen_width, &screen_height)) {
    ++stats_.declared_unknown;
    return false;
  }
  const uint32_t width = (size_bits >> 18) + 1;
  const uint32_t height = ((size_bits >> 3) & 0x7FFFu) + 1;
  for (uint32_t k = 0; k <= kMaxScreenShift; ++k) {
    if (width == (screen_width >> k) && height == (screen_height >> k)) {
      return true;
    }
  }
  return false;
}

Scale EdramTargets::ScaleForDeclared(uint32_t size_bits) {
  if (resolution_scale_.unit()) {
    return Scale{};
  }
  return IsScreenImage(size_bits) ? resolution_scale_ : Scale{};
}

bool EdramTargets::IsSceneKey(const EdramKey& key) {
  // The main view in play (temporal_identities.h): colour, or depth in the scene's layer. The 4x
  // identities of the pause menu and the map are not the scene's.
  if (key.depth == 0) {
    const uint32_t samples = temporal::MainColorSamples(key);
    return samples != 0 && temporal::IsPlaySamples(samples);
  }
  const uint32_t samples = temporal::MainDepthSamples(key);
  return samples != 0 && temporal::IsPlaySamples(samples) && key.reserved == 0;
}

Scale EdramTargets::ScaleForUse(const EdramKey& key, uint32_t size_bits) {
  if (IsSceneKey(key) && SceneApart() && IsScreenImage(size_bits)) {
    return scene_scale_;
  }
  return ScaleForDeclared(size_bits);
}

uint8_t EdramTargets::DepthLayer(const EdramKey& depth, const EdramKey* color0) {
  // Only the 1x main depth shares its key with another phase of the frame (a 2x one is the
  // scene's alone, a 4x one the pause menu's and the map's).
  if (temporal::MainDepthSamples(depth) != 1 || !SceneApart()) {
    return 0;
  }
  if (color0) {
    return temporal::MainColorSamples(*color0) != 0 ? 0 : 1;
  }
  return scene_phase_ ? 0 : 1;
}

void EdramTargets::DecideScene() {
  if (scene_decided_) {
    return;
  }
  scene_decided_ = true;
  scene_scale_ = resolution_scale_;
  // The upscaler as the start found it: this waits for its probe, if that has not ended (the
  // game's own thread asked the same question when it made its renderer, so it has).
  const StartUpscale start = GetStartUpscale();
  const SceneScaleChoice choice = ChooseSceneScale(resolution_scale_, start.ratio);
  scene_scale_ = choice.scale;
  if (!start.runs) {
    return;
  }
  if (scene_scale_ == resolution_scale_) {
    REXLOG_INFO("[gpu] edram: the 3D scene is made at the output's {}x ({} {}: not smaller than "
                "the output)",
                resolution_scale_, UpscalerName(start.upscaler),
                ModeInfoOf(start.upscaler, start.mode).name);
    return;
  }
  REXLOG_INFO("[gpu] edram: the 3D scene (the main view's colour and depth in play) is made at "
              "{}x, {}x{} of the guest's 1280x720, for the output at {}x, {}x{}: {} {} ({}%, "
              "ratio {:.2f}; the nearest step of the scale grid gives {:.3f}{}). The pause menu's "
              "and the map's views, the post chain, the tone map and the UI stay at {}x, and the "
              "upscaler makes the output from the scene. Fixed until the game is started again",
              scene_scale_, scene_scale_.Edge(kAssumedScreenWidth),
              scene_scale_.Edge(kAssumedScreenHeight), resolution_scale_,
              resolution_scale_.Edge(kAssumedScreenWidth),
              resolution_scale_.Edge(kAssumedScreenHeight), UpscalerName(start.upscaler),
              ModeInfoOf(start.upscaler, start.mode).name,
              ModeInfoOf(start.upscaler, start.mode).percent, start.ratio,
              choice.actual_ratio(resolution_scale_),
              choice.limited ? "; limited, 0.5x is the smallest scale tried" : "",
              resolution_scale_);
}

uint32_t EdramTargets::HostSamplesFor(const EdramKey& key, uint32_t size_bits) {
  if (key.samples <= 1 || msaa_level_ <= key.samples) {
    return key.samples;
  }
  return IsScreenImage(size_bits) ? msaa_level_ : key.samples;
}

void EdramTargets::CheckDeclaredScale(Target& target, uint32_t size_bits) {
  const Scale scale = ScaleForUse(target.key, size_bits);
  if (scale == target.scale) {
    target.declared_size_bits = size_bits;
    return;
  }
  ++stats_.scale_conflicts;
  if (std::find(scale_conflicts_reported_.begin(), scale_conflicts_reported_.end(), target.key) ==
      scale_conflicts_reported_.end()) {
    scale_conflicts_reported_.push_back(target.key);
    REXLOG_ERROR("[gpu] edram: {} was made at {}x and is now bound through a surface declared "
                 "{}x{}, which classifies at {}x; it keeps {}x (TODO: an identity used as an "
                 "image of the screen and as a target of its own size; reported once per "
                 "identity)",
                 DescribeEdramKey(target.key), target.scale, (size_bits >> 18) + 1,
                 ((size_bits >> 3) & 0x7FFFu) + 1, scale, target.scale);
  }
  target.declared_size_bits = size_bits;  // reported: the next uses of this size stay quiet
}

std::string DescribeEdramKey(const EdramKey& k) {
  const format::Mapping m = k.depth ? format::DepthTargetFormat(k.format)
                                    : format::ColorTargetFormat(k.format);
  return std::format("{} {}@{} pitch {} {}x{}", k.depth ? "depth" : "colour", m.guest_name,
                     k.base_tiles, k.pitch, k.samples, k.reserved ? " (the UI's layer)" : "");
}

void EncodeColorClear(uint32_t color_format, int32_t exp_bias, const float color[4],
                      float out[4]) {
  float scale = std::ldexp(1.0f, exp_bias);
  if (color_format == 4 || color_format == 5) {
    scale *= 1.0f / 32.0f;  // k_16_16, k_16_16_16_16: SNORM +-1 on the host = +-32
  }
  for (uint32_t c = 0; c < 4; ++c) {
    out[c] = color[c] * scale;
    if (color_format == 1) {
      out[c] = std::clamp(out[c], 0.0f, 1.0f);  // k_8_8_8_8_GAMMA, kept linear
    }
  }
}

size_t EdramTargets::KeyHash::operator()(const EdramKey& k) const {
  const uint64_t v = uint64_t(k.base_tiles) | (uint64_t(k.pitch) << 16) |
                     (uint64_t(k.depth) << 32) | (uint64_t(k.format) << 40) |
                     (uint64_t(k.samples) << 48) | (uint64_t(k.reserved) << 56);
  return size_t(v * 0x9E3779B97F4A7C15ull);
}

bool EdramTargets::FramebufferKey::operator==(const FramebufferKey& o) const {
  if (color_count != o.color_count) {
    return false;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    if (targets[i] != o.targets[i] || generations[i] != o.generations[i]) {
      return false;
    }
  }
  return true;
}

size_t EdramTargets::FramebufferKeyHash::operator()(const FramebufferKey& k) const {
  return size_t(XXH3_64bits(&k, sizeof(k)));
}

EdramTargets::~EdramTargets() {
  Shutdown();
}

void EdramTargets::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                              const RenderScaleChoice& scale) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  resolution_scale_ = scale.scale;
  scale_from_window_ = scale.from_window();
  // The scene's scale is decided on the first use of one of its identities (DecideScene).
  scene_scale_ = resolution_scale_;
  scene_decided_ = false;
  scene_phase_ = true;
  if (!resolution_scale_.unit()) {
    REXLOG_INFO("[gpu] edram: the images of the screen at {}x the guest's size ({}x{} from "
                "1280x720)",
                resolution_scale_, resolution_scale_.Edge(1280), resolution_scale_.Edge(720));
  }
  // The host MSAA level, chosen at start, within what the device can do for
  // every format an image of the screen is stored as.
  {
    std::string value = REXCVAR_GET(mcla_gpu_msaa);
    std::string v;
    for (unsigned char c : value) {
      if (!std::isspace(c)) v.push_back(char(std::tolower(c)));
    }
    if (!v.empty() && v.back() == 'x') v.pop_back();
    uint32_t wanted = 2;
    if (v == "4") {
      wanted = 4;
    } else if (v == "8") {
      wanted = 8;
    } else if (v != "2" && v != "off" && v != "1") {
      REXLOG_ERROR("[gpu] edram: mcla_gpu_msaa = '{}' is not off, 2, 4 or 8; the console's 2x "
                   "is kept",
                   value);
    }
    // off is the game's own switch (Patch_DisableMSAA): its targets come 1x.
    plume::RenderDevice* rd = device.device();
    uint32_t supported = 0xFFu;
    for (plume::RenderFormat f :
         {plume::RenderFormat::R8G8B8A8_UNORM, plume::RenderFormat::R16G16B16A16_FLOAT,
          plume::RenderFormat::D32_FLOAT_S8_UINT}) {
      supported &= uint32_t(rd->getSampleCountsSupported(f));
    }
    uint32_t level = wanted;
    while (level > 2 && (supported & level) == 0) {
      level >>= 1;
    }
    if (level != wanted) {
      REXLOG_ERROR("[gpu] edram: MSAA {}x asked, {}x used (device sample counts {:#x} for the "
                   "screen formats)",
                   wanted, level, supported);
    }
    msaa_level_ = level;
    if (msaa_level_ > 2) {
      REXLOG_INFO("[gpu] edram: the images of the screen the game multisamples get {} samples on "
                  "the host (mcla_gpu_msaa)",
                  msaa_level_);
    }
  }
  // The detector and its checks are chosen at start.
  aliasing_ = REXCVAR_GET(mcla_gpu_edram_aliasing);
  if (!aliasing_) {
    return;
  }
  for (auto& owners : tile_owner_) {
    owners.fill(kNoOwner);
  }
  capture_ = REXCVAR_GET(mcla_gpu_edram_capture);
  if (!REXCVAR_GET(mcla_gpu_coverage_queries)) {
    REXLOG_INFO("[gpu] edram: inheritance detector on; opaque draws it takes to cover their "
                "scissor go unchecked (mcla_gpu_coverage_queries off)");
    return;
  }
  if (!device.capabilities().occlusionQueries) {
    REXLOG_ERROR("[gpu] edram: mcla_gpu_coverage_queries needs occlusion queries that count "
                 "samples exactly, which this device has not: opaque draws the detector takes "
                 "to cover their scissor go unchecked");
    return;
  }
  coverage_pools_.resize(kFramesInFlight);
  coverage_checks_.resize(kFramesInFlight);
  coverage_capacity_ = 256;
  coverage_ = true;
  for (uint32_t slot = 0; slot < kFramesInFlight && coverage_; ++slot) {
    coverage_pools_[slot] = device.CreateQueryPool(coverage_capacity_, "edram coverage checks",
                                                   plume::RenderQueryType::OCCLUSION);
    coverage_ = coverage_pools_[slot] != nullptr;
  }
  if (!coverage_) {
    coverage_pools_.clear();
    coverage_checks_.clear();
    REXLOG_ERROR("[gpu] edram: the coverage checks' query pools could not be made: opaque draws "
                 "the detector takes to cover their scissor go unchecked");
  }
}

void EdramTargets::Shutdown() {
  // The GPU is idle and the heap goes next: the views die without handing
  // their slots back.
  framebuffers_.clear();
  targets_.clear();
  failed_.clear();
  ++change_serial_;
  for (auto& owners : tile_owner_) {
    owners.fill(kNoOwner);
  }
  owner_keys_.clear();
  epoch_ = 1;
  reported_pairs_.clear();
  listed_takes_.clear();
  for (auto& taint : tile_taint_) {
    taint.fill(0);
  }
  inherited_resolves_.clear();
  reach_reported_.clear();
  tainted_reported_.clear();
  reach_seen_.fill(false);
  request_capture_ = nullptr;
  capture_ = 0;
  captures_ = 0;
  tiling_ = false;
  pending_.clear();
  coverage_pools_.clear();
  coverage_checks_.clear();
  coverage_capacity_ = 0;
  coverage_wanted_ = 0;
  coverage_reported_.clear();
  aliasing_ = false;
  coverage_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
}

void EdramTargets::ReleaseViews(Target& t) {
  if (t.sample_slot != 0xFFFFFFFFu) {
    heap_->ReleaseTexture(t.sample_slot, TextureDimension::k2D);
    t.sample_slot = 0xFFFFFFFFu;
  }
  if (t.stencil_slot != 0xFFFFFFFFu) {
    heap_->ReleaseTexture(t.stencil_slot, TextureDimension::k2D);
    t.stencil_slot = 0xFFFFFFFFu;
  }
  frames_->Retire(std::move(t.sample_view));
  frames_->Retire(std::move(t.stencil_view));
}

bool EdramTargets::SampleSlots(Target& t, uint32_t* sample_slot, uint32_t* stencil_slot) {
  if (!t.texture) {
    return false;
  }
  if (t.views_generation != t.generation) {
    ReleaseViews(t);
    t.views_generation = t.generation;
  }
  if (t.sample_slot == 0xFFFFFFFFu) {
    // The texture's own format: Plume views a depth format through its depth
    // plane (R32_FLOAT_X8X24 / R24_UNORM_X8 on D3D12, the DEPTH aspect on Vulkan).
    plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(t.format);
    desc.mipLevels = 1;
    t.sample_view = device_->CreateTextureView(*t.texture, desc, "edram shader view");
    if (!t.sample_view) {
      return false;
    }
    t.sample_slot = heap_->AllocateTexture(*t.texture, *t.sample_view);
    if (t.sample_slot == 0xFFFFFFFFu) {
      return false;
    }
  }
  if (t.key.depth && t.stencil_slot == 0xFFFFFFFFu) {
    plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(t.format);
    desc.mipLevels = 1;
    desc.planeSlice = 1;
    t.stencil_view = device_->CreateTextureView(*t.texture, desc, "edram stencil view");
    if (!t.stencil_view) {
      return false;
    }
    t.stencil_slot = heap_->AllocateTexture(*t.texture, *t.stencil_view);
    if (t.stencil_slot == 0xFFFFFFFFu) {
      return false;
    }
  }
  *sample_slot = t.sample_slot;
  *stencil_slot = t.key.depth ? t.stencil_slot : 0xFFFFFFFFu;
  return true;
}

plume::RenderFramebuffer* EdramTargets::ClearColor(plume::RenderCommandList* list, Target& t,
                                                   plume::RenderRect rect, const float value[4]) {
  rect.right = std::min(rect.right, int32_t(t.width));
  rect.bottom = std::min(rect.bottom, int32_t(t.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return nullptr;
  }
  if (aliasing_) {
    // Every bit of every texel in the rectangle: its tiles are this
    // identity's from now on.
    Accesses access;
    access.aspect[kMain] = Access::kTake;
    access.aspect[kStencil] = Access::kTake;
    if (!Settled(t, rect, access)) {
      EdramEvent event;
      event.kind = EdramEvent::Kind::kClear;
      Note(t, rect, access, event);
    }
  }
  Transition(list, t, plume::RenderTextureLayout::COLOR_WRITE);
  Target* only[1] = {&t};
  plume::RenderFramebuffer* framebuffer = Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  // The rectangle is the guest's (the tile map above); the host clears it
  // at the target's scale.
  const plume::RenderRect host = ScaledRect(rect, t.scale);
  list->clearColor(0, plume::RenderColor(value[0], value[1], value[2], value[3]), &host, 1);
  return framebuffer;
}

plume::RenderFramebuffer* EdramTargets::ClearDepthStencil(plume::RenderCommandList* list,
                                                          Target& t, plume::RenderRect rect,
                                                          bool depth, bool stencil, float z,
                                                          uint32_t stencil_value) {
  rect.right = std::min(rect.right, int32_t(t.width));
  rect.bottom = std::min(rect.bottom, int32_t(t.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top || (!depth && !stencil)) {
    return nullptr;
  }
  if (aliasing_) {
    // Each aspect cleared is this identity's from now on; one not cleared
    // keeps its owner.
    Accesses access;
    access.aspect[kMain] = depth ? Access::kTake : Access::kNone;
    access.aspect[kStencil] = stencil ? Access::kTake : Access::kNone;
    if (!Settled(t, rect, access)) {
      EdramEvent event;
      event.kind = EdramEvent::Kind::kClear;
      Note(t, rect, access, event);
    }
  }
  Transition(list, t, plume::RenderTextureLayout::DEPTH_WRITE);
  plume::RenderFramebuffer* framebuffer = Framebuffer(nullptr, 0, &t);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  const plume::RenderRect host = ScaledRect(rect, t.scale);
  list->clearDepthStencil(depth, stencil, z, stencil_value & 0xFFu, &host, 1);
  return framebuffer;
}

namespace {

// The host texture of an identity: the guest's size at `scale`, `host_samples` samples. Null when
// the device cannot make it (logged by the device).
std::unique_ptr<plume::RenderTexture> MakeTargetTexture(Device& device, const EdramKey& k,
                                                        const format::Mapping& m, Scale scale,
                                                        uint32_t host_samples, uint32_t height) {
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      scale.Edge(k.pitch), scale.Edge(height), 1, m.format,
      k.depth ? plume::RenderTextureFlag::DEPTH_TARGET : plume::RenderTextureFlag::RENDER_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(host_samples);
  const std::string name =
      std::format("edram {} {}@{} pitch {} {}x scale {} host {}x", k.depth ? "depth" : "colour",
                  m.guest_name, k.base_tiles, k.pitch, k.samples, scale, host_samples);
  return device.CreateTexture(desc, name.c_str());
}

}  // namespace

bool EdramTargets::Create(Target& target, uint32_t height) {
  const EdramKey& k = target.key;
  const format::Mapping m = k.depth ? format::DepthTargetFormat(k.format)
                                    : format::ColorTargetFormat(k.format);
  if (m.status != format::Status::kOk) {
    return false;
  }
  // The host texture: the guest's size at the target's scale.
  std::unique_ptr<plume::RenderTexture> texture =
      MakeTargetTexture(*device_, k, m, target.scale, target.host_samples, height);
  if (!texture) {
    return false;
  }
  target.texture = std::move(texture);
  target.format = m.format;
  target.width = k.pitch;
  target.height = height;
  target.layout = plume::RenderTextureLayout::UNKNOWN;
  ++target.generation;
  ++change_serial_;
  return true;
}

bool EdramTargets::SetSceneScale(Scale scale, plume::RenderCommandList* list) {
  DecideScene();  // the start's decision first: a change before the first use must not be undone by it
  if (scale == scene_scale_) {
    return true;
  }
  if (!device_ || !frames_ || !list || scale.none()) {
    return false;
  }
  // The new textures first, all of them: nothing is changed unless every one can be made.
  struct Remade {
    Target* target = nullptr;
    format::Mapping mapping;
    std::unique_ptr<plume::RenderTexture> texture;
  };
  std::vector<Remade> remade;
  for (const auto& [key, target] : targets_) {
    if (!IsSceneKey(key) || !target->texture || target->scale == scale) {
      continue;
    }
    Remade r;
    r.target = target.get();
    r.mapping = key.depth ? format::DepthTargetFormat(key.format)
                          : format::ColorTargetFormat(key.format);
    if (r.mapping.status == format::Status::kOk) {
      r.texture = MakeTargetTexture(*device_, key, r.mapping, scale, target->host_samples,
                                    target->height);
    }
    if (!r.texture) {
      REXLOG_ERROR("[gpu] edram: the 3D scene could not be moved from {}x to {}x: {} could not be "
                   "made at the new size (the scene stays at {}x)",
                   scene_scale_, scale, DescribeEdramKey(key), scene_scale_);
      return false;
    }
    remade.push_back(std::move(r));
  }

  const Scale before = scene_scale_;
  uint64_t old_bytes = 0;
  uint64_t new_bytes = 0;
  for (Remade& r : remade) {
    Target& t = *r.target;
    old_bytes += TextureBytes(t.scale.Edge(t.width), t.scale.Edge(t.height), 1, t.format,
                              t.host_samples);
    // The framebuffers that name it, the shader views and their slots, then the texture, each after
    // the lists that still use them (FrameRing serials); the same as a growth, with a new scale.
    RetireFramebuffersOf(&t);
    ReleaseViews(t);
    std::unique_ptr<plume::RenderTexture> old = std::move(t.texture);
    t.texture = std::move(r.texture);
    t.format = r.mapping.format;
    t.scale = scale;
    t.layout = plume::RenderTextureLayout::UNKNOWN;
    ++t.generation;
    t.views_generation = t.generation;
    new_bytes += TextureBytes(scale.Edge(t.width), scale.Edge(t.height), 1, t.format, t.host_samples);
    frames_->Retire(std::move(old));
    // A new colour target or depth buffer is cleared whole before anything else touches it
    // (D3D12 debug layer id 1422), as every new target is.
    Clear(t, list);
    ++stats_.rescaled;
  }
  for (const auto& [key, target] : targets_) {
    if (IsSceneKey(key)) {
      target->scale = scale;  // also a target released for want of memory: Get makes it at this scale
    }
  }
  if (old_bytes != 0) {
    device_->NoteReleased(old_bytes);
  }
  scene_scale_ = scale;
  scene_decided_ = true;
  // What was memoized from the targets (a draw's scale and framebuffers, the depth's layer, which
  // follows whether the scene is apart from the images of the screen) is stale.
  ++change_serial_;
  REXLOG_INFO("[gpu] edram: the 3D scene is now made at {}x, {}x{} of the guest's 1280x720 (was {}x): "
              "{} target(s) remade, {:.1f} MiB (was {:.1f} MiB){}",
              scale, scale.Edge(kAssumedScreenWidth), scale.Edge(kAssumedScreenHeight), before,
              remade.size(), double(new_bytes) / 1048576.0, double(old_bytes) / 1048576.0,
              scale == resolution_scale_ ? "; the images of the screen's scale again" : "");
  return true;
}

void EdramTargets::Clear(Target& target, plume::RenderCommandList* list) {
  if (target.key.depth) {
    Transition(list, target, plume::RenderTextureLayout::DEPTH_WRITE);
    plume::RenderFramebuffer* framebuffer = Framebuffer(nullptr, 0, &target);
    if (framebuffer) {
      list->setFramebuffer(framebuffer);
      list->clearDepthStencil(true, true, 0.0f, 0);
    }
  } else {
    Transition(list, target, plume::RenderTextureLayout::COLOR_WRITE);
    Target* only[1] = {&target};
    plume::RenderFramebuffer* framebuffer = Framebuffer(only, 1, nullptr);
    if (framebuffer) {
      list->setFramebuffer(framebuffer);
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
  }
  list->setFramebuffer(nullptr);
  ++stats_.cleared;
}

EdramTargets::Target* EdramTargets::Get(const EdramKey& key, uint32_t height, Scale scale,
                                        uint32_t size_bits, plume::RenderCommandList* list,
                                        bool* framebuffer_unset) {
  if (key.pitch == 0 || key.pitch > kMaxExtent) {
    return nullptr;
  }
  const uint32_t wanted =
      std::min(AlignUp(std::max<uint32_t>(height, 1), kHeightAlignment), kMaxExtent);
  auto it = targets_.find(key);
  if (it == targets_.end()) {
    if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) {
      return nullptr;
    }
    auto target = std::make_unique<Target>();
    target->key = key;
    // An image of the screen at the internal scale, the scene at its own (when the scene is drawn
    // smaller: the scale of a use is ScaleOf's, which gave no other); anything else at the
    // guest's size.
    target->scale = (scale == resolution_scale_ || (scene_decided_ && scale == scene_scale_))
                        ? scale
                        : Scale{};
    target->host_samples = HostSamplesFor(key, size_bits);
    if (!Create(*target, wanted)) {
      failed_.push_back(key);
      REXLOG_ERROR("[gpu] edram: no host target for {} format {} at tile {} pitch {} {}x "
                   "(reported once)",
                   key.depth ? "depth" : "colour", key.format, key.base_tiles, key.pitch,
                   key.samples);
      return nullptr;
    }
    ++stats_.created;
    if (!target->scale.unit()) {
      ++stats_.made_scaled;
    } else {
      ++stats_.made_unscaled;
    }
    if (target->host_samples != key.samples) {
      ++stats_.made_host_msaa;
      REXLOG_INFO("[gpu] edram: {} made with {} samples on the host (an image of the screen, "
                  "first use {} rows)",
                  DescribeEdramKey(key), target->host_samples, height);
    }
    if (!resolution_scale_.unit() || (scene_decided_ && !scene_scale_.unit())) {
      // The census of the rule: what each identity became, once.
      REXLOG_INFO("[gpu] edram: {} made at {}x ({}; declared {}x{}, first use {} rows)",
                  DescribeEdramKey(key), target->scale,
                  scene_decided_ && scene_scale_ != resolution_scale_ && IsSceneKey(key)
                      ? "the 3D scene"
                  : !target->scale.unit() ? "an image of the screen"
                                          : "the guest's size",
                  size_bits ? (size_bits >> 18) + 1 : 0u,
                  size_bits ? ((size_bits >> 3) & 0x7FFFu) + 1 : 0u, height);
    }
    AssignOwner(*target);
    it = targets_.emplace(key, std::move(target)).first;
    if (REXCVAR_GET(mcla_gpu_ghost_log_interval) != 0) {
      const Target& made = *it->second;
      REXLOG_INFO("[gpu] ghost target create frame {} {} target {} texture {} height {} gen {}",
                  frame_, DescribeEdramKey(key), static_cast<const void*>(&made),
                  static_cast<const void*>(made.texture.get()), made.height, made.generation);
    }
    Clear(*it->second, list);
    *framebuffer_unset = true;
  }
  Target& t = *it->second;
  t.last_frame = frame_;
  if (!t.texture) {
    // Released for want of video memory (Trim): made again at the size it had
    // when the identity was made, cleared as a new target's texture is.
    if (!Create(t, wanted)) {
      if (remake_failures_logged_ < 8) {
        ++remake_failures_logged_;
        REXLOG_ERROR("[gpu] edram: could not make the texture of {} again after it was released "
                     "for want of video memory",
                     DescribeEdramKey(key));
      }
      return nullptr;
    }
    ++stats_.remade;
    Clear(t, list);
    *framebuffer_unset = true;
    return &t;
  }
  if (t.height >= wanted) {
    return &t;
  }
  // Grow: a new texture, the old one copied into its top rows.
  std::unique_ptr<plume::RenderTexture> old = std::move(t.texture);
  const plume::RenderTextureLayout old_layout = t.layout;
  const uint32_t old_height = t.height;
  if (!Create(t, wanted)) {
    t.texture = std::move(old);
    t.height = old_height;
    t.layout = old_layout;
    // Asked again by every draw that needs the rows; the device does not ask the driver again
    // for a while (Device::kBackoffFrames), and the first 8 are logged.
    if (grow_failures_logged_ < 8) {
      ++grow_failures_logged_;
      REXLOG_ERROR("[gpu] edram: could not grow target at tile {} pitch {} to {} rows",
                   key.base_tiles, key.pitch, wanted);
    }
    return &t;  // the old size still draws everything but the new rows
  }
  // The whole new texture is initialized, then the old rows copied over it.
  Clear(t, list);
  *framebuffer_unset = true;
  {
    const plume::RenderTextureBarrier before[] = {
        plume::RenderTextureBarrier(old.get(), plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
    list->barriers(plume::RenderBarrierStage::COPY, before, 2);
    if (key.depth) {
      // Depth and stencil are separate planes on D3D12; copied one by one.
      for (uint32_t plane = 0; plane < 2; ++plane) {
        list->copyTextureRegion(
            plume::RenderTextureCopyLocation::SubresourcePlane(t.texture.get(), 0, 0, plane),
            plume::RenderTextureCopyLocation::SubresourcePlane(old.get(), 0, 0, plane), 0, 0, 0,
            nullptr);
      }
    } else {
      list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get()),
                              plume::RenderTextureCopyLocation::Subresource(old.get()), 0, 0, 0,
                              nullptr);
    }
    t.layout = plume::RenderTextureLayout::COPY_DEST;
    stats_.barriers += 2;
  }
  ++stats_.grown;
  if (REXCVAR_GET(mcla_gpu_ghost_log_interval) != 0) {
    REXLOG_INFO("[gpu] ghost target grow frame {} {} target {} texture {} -> {} height {} -> {} "
                "gen {}",
                frame_, DescribeEdramKey(key), static_cast<const void*>(&t),
                static_cast<const void*>(old.get()), static_cast<const void*>(t.texture.get()),
                old_height, t.height, t.generation);
  }
  stats_.grown_bytes += uint64_t(t.width) * (wanted - old_height) * BytesPerPixel(t.format) *
                        t.host_samples;
  RetireFramebuffersOf(&t);
  // Views of the old texture: their slots go null before it dies.
  ReleaseViews(t);
  t.views_generation = t.generation;
  frames_->Retire(std::move(old));
  return &t;
}

void EdramTargets::AssignOwner(Target& t) {
  if (!aliasing_) {
    return;
  }
  // Tile columns of one row of tiles: the pitch in samples over 80 at 32 bpp,
  // doubled for a 64 bpp format (Xenia's RenderTargetKey::GetPitchTiles). 4x
  // doubles the samples across, 2x and 4x down.
  const uint32_t across = t.key.samples >= 4 ? 2 : 1;
  const bool wide = !t.key.depth && rex::graphics::xenos::IsColorRenderTargetFormat64bpp(
                                        rex::graphics::xenos::ColorRenderTargetFormat(t.key.format));
  t.columns_per_texel = wide ? 2 : 1;
  t.pitch_tiles = ((uint32_t(t.key.pitch) * across + 79) / 80) * t.columns_per_texel;
  if (owner_keys_.size() >= kNoOwner) {
    if (!owners_exhausted_logged_) {
      owners_exhausted_logged_ = true;
      REXLOG_ERROR("[gpu] edram: more than {} identities; the newer ones are not watched for "
                   "EDRAM inheritance (reported once)",
                   uint32_t(kNoOwner));
    }
    return;
  }
  t.owner = uint16_t(owner_keys_.size());
  owner_keys_.push_back(t.key);
}

void EdramTargets::TileSpan(const Target& t, const plume::RenderRect& px, uint32_t* col0,
                            uint32_t* col1, uint32_t* row0, uint32_t* row1) {
  // The rectangle in the identity's tiles (80x16 samples).
  const uint32_t across = (t.key.samples >= 4 ? 2 : 1) * t.columns_per_texel;
  const uint32_t down = t.key.samples >= 2 ? 2 : 1;
  *col0 = uint32_t(std::max(px.left, 0)) * across / 80;
  *col1 = std::min((uint32_t(std::max(px.right, 0)) * across + 79) / 80, t.pitch_tiles);
  *row0 = uint32_t(std::max(px.top, 0)) * down / 16;
  *row1 = (uint32_t(std::max(px.bottom, 0)) * down + 15) / 16;
}

EdramTargets::Inheritance EdramTargets::Apply(Target& t, const plume::RenderRect& px,
                                              const Accesses& access) {
  Inheritance result;
  if (t.owner == kNoOwner || px.right <= px.left || px.bottom <= px.top) {
    return result;
  }
  uint32_t col0 = 0, col1 = 0, row0 = 0, row1 = 0;
  TileSpan(t, px, &col0, &col1, &row0, &row1);
  ++stats_.walks;
  bool changed = false;
  bool own[kAspectCount] = {true, true};
  uint32_t depend_tiles = 0;
  uint32_t read_tiles = 0;
  uint32_t tainted_tiles = 0;
  bool tainted_read = false;
  uint16_t from = kNoOwner;
  uint16_t taken_from = kNoOwner;
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    const Access acc = access.aspect[a];
    if (acc == Access::kNone) {
      continue;
    }
    std::array<uint16_t, kTileCount>& owners = tile_owner_[a];
    std::array<uint8_t, kTileCount>& taint = tile_taint_[a];
    for (uint32_t row = row0; row < row1; ++row) {
      // The EDRAM's addressing wraps at its last tile.
      const uint32_t first = uint32_t(t.key.base_tiles) + row * t.pitch_tiles;
      for (uint32_t col = col0; col < col1; ++col) {
        const uint32_t tile = (first + col) & (kTileCount - 1);
        uint16_t& owner = owners[tile];
        uint8_t& marked = taint[tile];
        if (acc == Access::kTake) {
          // Every texel replaced: whatever the tile held is gone.
          if (marked) {
            marked = 0;
            ++stats_.untainted;
            changed = true;
          }
          if (owner == t.owner) {
            continue;
          }
          if (owner != kNoOwner) {
            ++stats_.tiles_taken;
            ++result.taken;
            if (taken_from == kNoOwner) {
              taken_from = owner;
              result.taken_owner = owner;
            }
          }
          owner = t.owner;
          changed = true;
          continue;
        }
        // kDepend or kRead: what the tile holds may reach the result.
        if (owner == t.owner) {
          if (marked) {
            // Its own tile, holding another identity's bytes since a write
            // kept them: walked again until something takes it.
            ++tainted_tiles;
            tainted_read |= acc == Access::kRead;
            own[a] = false;
          }
          continue;
        }
        if (owner != kNoOwner) {
          ++(acc == Access::kRead ? read_tiles : depend_tiles);
          if (from == kNoOwner) {
            from = owner;
            result.aspect = Aspect(a);
          }
        }
        if (acc == Access::kRead) {
          own[a] = false;
        } else {
          if (owner != kNoOwner) {
            // It keeps or blends another identity's bytes: they stay in the
            // tile (an unowned tile holds the EDRAM's undefined first bytes,
            // no identity's).
            if (!marked) {
              marked = 1;
              ++stats_.tainted;
            }
            own[a] = false;
          }
          owner = t.owner;
          changed = true;
        }
      }
    }
  }
  if (changed) {
    ++epoch_;
  }
  result.tainted = tainted_tiles;
  stats_.tainted_reads += tainted_tiles;
  if (tainted_read && tainted_reported_.size() < kReportedPairs &&
      std::find(tainted_reported_.begin(), tainted_reported_.end(), t.owner) ==
          tainted_reported_.end()) {
    tainted_reported_.push_back(t.owner);
    result.tainted_new = true;
  }
  // What is known now, per aspect: the larger rectangle of the epoch.
  const auto area = [](const plume::RenderRect& r) {
    return int64_t(r.right - r.left) * int64_t(r.bottom - r.top);
  };
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    const Access acc = access.aspect[a];
    if (acc == Access::kNone) {
      continue;
    }
    if (own[a]) {
      if (t.own_epoch[a] != epoch_ || area(px) > area(t.own_rect[a])) {
        t.own_rect[a] = px;
      }
      t.own_epoch[a] = epoch_;
    } else if (acc == Access::kRead) {
      if (t.read_epoch[a] != epoch_ || area(px) > area(t.read_rect[a])) {
        t.read_rect[a] = px;
      }
      t.read_epoch[a] = epoch_;
    }
  }
  if (taken_from != kNoOwner) {
    const uint32_t pair = uint32_t(taken_from) << 16 | t.owner;
    if (listed_takes_.size() < 32 &&
        std::find(listed_takes_.begin(), listed_takes_.end(), pair) == listed_takes_.end()) {
      listed_takes_.push_back(pair);
      result.taken_from = &owner_keys_[taken_from];
    }
  }
  result.tiles = depend_tiles + read_tiles;
  if (result.tiles != 0) {
    if (depend_tiles != 0) {
      ++stats_.depend_events;
    } else {
      ++stats_.read_events;
    }
    stats_.depend_tiles += depend_tiles;
    stats_.read_tiles += read_tiles;
    const uint32_t pair = uint32_t(from) << 16 | t.owner;
    if (reported_pairs_.size() < kReportedPairs &&
        std::find(reported_pairs_.begin(), reported_pairs_.end(), pair) == reported_pairs_.end()) {
      reported_pairs_.push_back(pair);
      result.from = &owner_keys_[from];
    }
  }
  return result;
}

void EdramTargets::Note(Target& t, const plume::RenderRect& px, const Accesses& access,
                        const EdramEvent& event, Target* const* outputs,
                        uint32_t output_count) {
  output_count = outputs ? std::min<uint32_t>(output_count, 4) : 0;
  if (tiling_) {
    // A repeat of the access kept last changes nothing when the tiles replay
    // it: one is enough. A resolve is never a repeat: each copy's destination
    // is marked on its own (reach).
    if (!pending_.empty() && event.kind != EdramEvent::Kind::kResolve) {
      const Pending& last = pending_.back();
      bool same = last.target == &t && last.access == access && last.rect.left == px.left &&
                  last.rect.top == px.top && last.rect.right == px.right &&
                  last.rect.bottom == px.bottom && last.output_count == output_count &&
                  last.event.kind == event.kind;
      for (uint32_t i = 0; i < output_count && same; ++i) {
        same = last.outputs[i] == outputs[i];
      }
      if (same) {
        return;
      }
    }
    Pending p{&t, px, access, event};
    for (uint32_t i = 0; i < output_count; ++i) {
      p.outputs[i] = outputs[i];
    }
    p.output_count = output_count;
    pending_.push_back(p);
    ++stats_.tiled_kept;
    return;
  }
  Account(t, px, access, event, outputs, output_count, -1);
}

void EdramTargets::Account(Target& t, const plume::RenderRect& px, const Accesses& access,
                           const EdramEvent& event, Target* const* outputs,
                           uint32_t output_count, int32_t tile) {
  const Inheritance found = Apply(t, px, access);
  if (output_count != 0 && (found.tiles != 0 || found.tainted != 0)) {
    // A test over inherited bytes decides what the draw writes.
    for (uint32_t i = 0; i < output_count; ++i) {
      Taint(*outputs[i], px);
    }
  }
  Report(t, access, found, event, tile);
}

void EdramTargets::Taint(const Target& t, const plume::RenderRect& px) {
  if (t.owner == kNoOwner || px.right <= px.left || px.bottom <= px.top) {
    return;
  }
  uint32_t col0 = 0, col1 = 0, row0 = 0, row1 = 0;
  TileSpan(t, px, &col0, &col1, &row0, &row1);
  bool changed = false;
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    for (uint32_t row = row0; row < row1; ++row) {
      const uint32_t first = uint32_t(t.key.base_tiles) + row * t.pitch_tiles;
      for (uint32_t col = col0; col < col1; ++col) {
        const uint32_t tile = (first + col) & (kTileCount - 1);
        // Only what the output wrote: a tile it does not own is not its result.
        if (tile_owner_[a][tile] == t.owner && !tile_taint_[a][tile]) {
          tile_taint_[a][tile] = 1;
          ++stats_.tainted;
          changed = true;
        }
      }
    }
  }
  if (changed) {
    ++epoch_;  // what was known clean of these tiles is not any more
  }
}

void EdramTargets::BeginTiling() {
  if (!aliasing_) {
    return;
  }
  if (tiling_ && !tiling_mismatch_logged_) {
    tiling_mismatch_logged_ = true;
    REXLOG_ERROR("[gpu] edram: a tiled section began inside another; the EDRAM ownership of "
                 "the first is noted with the second's tiles (reported once)");
  }
  tiling_ = true;
  ++stats_.tiled_sections;
}

void EdramTargets::EndTiling(const EdramTile* tiles, uint32_t count) {
  if (!aliasing_) {
    return;
  }
  if (!tiling_) {
    if (!tiling_mismatch_logged_) {
      tiling_mismatch_logged_ = true;
      REXLOG_ERROR("[gpu] edram: a tiled section ended that did not begin ("
                   "reported once)");
    }
    return;
  }
  tiling_ = false;
  if (count == 0 && !pending_.empty()) {
    if (!tiling_mismatch_logged_) {
      tiling_mismatch_logged_ = true;
      REXLOG_ERROR("[gpu] edram: a tiled section ended without tiles; its {} accesses are noted "
                   "in surface pixels, as if untiled (TODO M5.2d; reported once)",
                   pending_.size());
    }
    for (const Pending& p : pending_) {
      if (!SettledFor(p, p.rect)) {
        Account(*p.target, p.rect, p.access, p.event, p.outputs, p.output_count, -1);
      }
    }
  }
  // The GPU runs the whole section for one tile before the next.
  for (uint32_t i = 0; i < count; ++i) {
    const EdramTile& tile = tiles[i];
    for (const Pending& p : pending_) {
      plume::RenderRect r(std::max(p.rect.left, tile.rect.left),
                          std::max(p.rect.top, tile.rect.top),
                          std::min(p.rect.right, tile.rect.right),
                          std::min(p.rect.bottom, tile.rect.bottom));
      if (r.right <= r.left || r.bottom <= r.top) {
        continue;
      }
      r.left -= tile.origin_x;
      r.right -= tile.origin_x;
      r.top -= tile.origin_y;
      r.bottom -= tile.origin_y;
      ++stats_.tiled_noted;
      if (!SettledFor(p, r)) {
        Account(*p.target, r, p.access, p.event, p.outputs, p.output_count, int32_t(i));
      }
    }
  }
  pending_.clear();
}

bool EdramTargets::SettledFor(const Pending& p, const plume::RenderRect& r) const {
  // A copy, or a test with outputs, of bytes already counted still carries
  // them into its result (reach).
  return p.event.kind == EdramEvent::Kind::kResolve || p.output_count != 0
             ? SettledClean(*p.target, r, p.access)
             : Settled(*p.target, r, p.access);
}

void EdramTargets::Report(const Target& t, const Accesses& access, const Inheritance& found,
                          const EdramEvent& e, int32_t tile) {
  if (found.taken != 0 && e.check == kNoCheck && e.kind == EdramEvent::Kind::kDraw) {
    ++stats_.takes_unchecked;
  }
  if (found.taken != 0 && e.check != kNoCheck) {
    // Rests on the draw's extent until its check is read.
    std::vector<CoverageCheck>& checks = coverage_checks_[e.check >> 24];
    const uint32_t index = e.check & 0xFFFFFFu;
    if (index < checks.size()) {
      CoverageCheck& check = checks[index];
      if (check.taken_owner == kNoOwner) {
        check.taken_owner = found.taken_owner;
        check.target = t.owner;
      }
      check.taken += found.taken;
    }
  }
  if (e.kind == EdramEvent::Kind::kResolve && e.resolve != 0 &&
      (found.tiles != 0 || found.tainted != 0)) {
    // Its destination holds what the console would copy from another
    // identity: the ResolveCache marks it and watches its readers.
    inherited_resolves_.push_back(e.resolve);
    ++stats_.inherited_resolves;
  }
  if (!found.from && !found.taken_from && !found.tainted_new) {
    return;
  }
  std::string what;
  switch (e.kind) {
    case EdramEvent::Kind::kDraw:
      what = std::format(
          "a draw: {} {} n={}, blend {:08X} mask {:X}, RB_DEPTHCONTROL {:08X}, RB_COLORCONTROL "
          "{:08X}, stencil {:08X}/{:08X}, AA mask {:X}, scissor ({}, {})-({}, {}), VS {:016X} PS "
          "{:016X}",
          e.draw_kind, e.primitive, e.count, e.blend, e.write_mask, e.depth_control,
          e.color_control, e.stencil, e.stencil_bf, e.aa_mask, e.scissor.left, e.scissor.top,
          e.scissor.right, e.scissor.bottom, e.vs, e.ps);
      break;
    case EdramEvent::Kind::kClear:
      what = "a clear";
      break;
    case EdramEvent::Kind::kResolve:
      what = std::format("a resolve into {:#010x}, texture format {} {}x{}", e.dest_page << 12,
                         e.dest_format, e.dest_width, e.dest_height);
      break;
  }
  const std::string where = tile < 0 ? std::string() : std::format(", tile {}", tile);
  if (found.from) {
    const char* aspect = t.key.depth ? (found.aspect == kMain ? "depth" : "stencil")
                                     : "colour words";
    const char* how = access.aspect[found.aspect] == Access::kRead ? "reads" : "keeps or blends";
    REXLOG_ERROR("[gpu] edram: {} {} the {} of {} aspect-tiles {} wrote last{}: on the console "
                 "it sees that identity's bytes, which the host does not carry over between "
                 "EDRAM identities (TODO M5.2d; pair reported once) -- {}",
                 DescribeEdramKey(t.key), how, aspect, found.tiles,
                 DescribeEdramKey(*found.from), where, what);
    RequestCapture(access.aspect[found.aspect] == Access::kRead ? kCaptureReadPair
                                                                 : kCaptureWritePair);
  }
  if (found.tainted_new) {
    REXLOG_ERROR("[gpu] edram: {} reads {} aspect-tiles of its own that hold another identity's "
                 "bytes (a write kept or blended them, or a test of them decided it){}: on the "
                 "console the result carries those bytes (TODO M5.2d; reported once per "
                 "identity) -- {}",
                 DescribeEdramKey(t.key), found.tainted, where, what);
  }
  if (found.taken_from && e.kind == EdramEvent::Kind::kDraw) {
    REXLOG_INFO("[gpu] edram: {} takes {} aspect-tiles of {}{} with an opaque draw taken to "
                "cover its scissor, {} (pair listed once) -- {}",
                DescribeEdramKey(t.key), found.taken, DescribeEdramKey(*found.taken_from), where,
                e.check != kNoCheck ? "checked by an occlusion query"
                : coverage_         ? "UNCHECKED (no query left this frame)"
                                    : "UNCHECKED (mcla_gpu_coverage_queries off)",
                what);
  }
}

void EdramTargets::NoteReach(Reach kind, uint64_t id, const std::function<std::string()>& describe) {
  ++stats_.reach[size_t(kind)];
  const uint64_t key = uint64_t(kind) << 62 | (id & ((uint64_t(1) << 62) - 1));
  if (reach_reported_.size() < kReportedPairs &&
      std::find(reach_reported_.begin(), reach_reported_.end(), key) == reach_reported_.end()) {
    reach_reported_.push_back(key);
    static const char* const kWhat[] = {"a fetch", "a texture decoded from the resolve memory",
                                        "a CPU lock"};
    REXLOG_ERROR("[gpu] edram: inherited bytes reach {}: {} -- on the console they are another "
                 "EDRAM identity's, which the host does not carry over (TODO M5.2d; reported "
                 "once)",
                 kWhat[size_t(kind)], describe());
  }
  if (!reach_seen_[size_t(kind)]) {
    reach_seen_[size_t(kind)] = true;
    RequestCapture(kCaptureReach);
  }
}

void EdramTargets::RequestCapture(uint32_t why) {
  if (!(capture_ & why) || !request_capture_ || captures_ >= 4) {
    return;
  }
  ++captures_;
  request_capture_();
  REXLOG_INFO("[gpu] edram: RenderDoc capture of the next frame requested ({} of 4, "
              "mcla_gpu_edram_capture bit {})",
              captures_, why);
}

uint32_t EdramTargets::ReserveCoverageCheck() {
  if (!coverage_) {
    return kNoCheck;
  }
  std::vector<CoverageCheck>& checks = coverage_checks_[slot_];
  if (checks.size() >= coverage_capacity_) {
    // The pools grow at their next frame; until then such draws depend.
    coverage_wanted_ = std::max(coverage_wanted_, coverage_capacity_ * 2);
    ++stats_.checks_wanting;
    return kNoCheck;
  }
  checks.emplace_back();
  return slot_ << 24 | uint32_t(checks.size() - 1);
}

void EdramTargets::DescribeCoverageCheck(uint32_t check, const EdramEvent& event) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  coverage_checks_[slot_][check & 0xFFFFFFu].event = event;
}

void EdramTargets::BeginCoverageCheck(plume::RenderCommandList* list, uint32_t check,
                                      uint64_t expected_samples) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  CoverageCheck& c = coverage_checks_[slot_][check & 0xFFFFFFu];
  c.expected = expected_samples;
  c.recorded = true;
  list->beginOcclusionQuery(coverage_pools_[slot_].get(), check & 0xFFFFFFu);
}

void EdramTargets::EndCoverageCheck(plume::RenderCommandList* list, uint32_t check) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  list->endOcclusionQuery(coverage_pools_[slot_].get(), check & 0xFFFFFFu);
}

void EdramTargets::HarvestCoverage(uint32_t slot) {
  std::vector<CoverageCheck>& checks = coverage_checks_[slot];
  if (checks.empty() || !coverage_pools_[slot]) {
    checks.clear();
    return;
  }
  coverage_pools_[slot]->queryResults();
  const uint64_t* passed = coverage_pools_[slot]->getResults();
  for (size_t i = 0; i < checks.size(); ++i) {
    const CoverageCheck& c = checks[i];
    if (!c.recorded) {
      ++stats_.checks_unrecorded;
      continue;
    }
    ++stats_.checks;
    if (passed[i] == c.expected) {
      ++stats_.checks_full;
      continue;
    }
    if (passed[i] > c.expected) {
      // Two triangles that overlap: the count proves nothing either way.
      ++stats_.checks_over;
      if (!coverage_over_logged_) {
        coverage_over_logged_ = true;
        REXLOG_ERROR("[gpu] edram: a draw taken to cover its scissor counted {} samples of {}: "
                     "its triangles overlap, and the count cannot tell whether it covered it "
                     "(reported once) -- {} {} n={}, VS {:016X} PS {:016X}",
                     passed[i], c.expected, c.event.draw_kind, c.event.primitive,
                     c.event.count, c.event.vs, c.event.ps);
      }
      if (c.taken == 0) {
        continue;
      }
    } else {
      ++stats_.checks_partial;
      if (c.taken == 0) {
        continue;  // nothing of another identity rested on it
      }
    }
    ++stats_.checks_partial_taken;
    const uint32_t pair = uint32_t(c.taken_owner) << 16 | c.target;
    if (c.taken_owner < owner_keys_.size() && c.target < owner_keys_.size() &&
        coverage_reported_.size() < kReportedPairs &&
        std::find(coverage_reported_.begin(), coverage_reported_.end(), pair) ==
            coverage_reported_.end()) {
      coverage_reported_.push_back(pair);
      const EdramEvent& e = c.event;
      REXLOG_ERROR(
          "[gpu] edram: an opaque draw on {} took {} aspect-tiles of {} as if it covered its "
          "scissor, and wrote {} of its {} samples: on the console what it left keeps that "
          "identity's bytes, which the host does not carry over between EDRAM identities (TODO "
          "M5.2d; pair reported once) -- {} {} n={}, scissor ({}, {})-({}, {}), VS {:016X} PS "
          "{:016X}",
          DescribeEdramKey(owner_keys_[c.target]), c.taken,
          DescribeEdramKey(owner_keys_[c.taken_owner]), passed[i], c.expected, e.draw_kind,
          e.primitive, e.count, e.scissor.left, e.scissor.top, e.scissor.right, e.scissor.bottom,
          e.vs, e.ps);
    }
  }
  checks.clear();
}

void EdramTargets::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  ++frame_;
  if (!coverage_ || slot >= coverage_checks_.size()) {
    return;  // no pool to read or reset
  }
  slot_ = slot;
  HarvestCoverage(slot);
  if (!list) {
    return;
  }
  if (coverage_wanted_ > coverage_capacity_ && device_) {
    // The other slots' pools grow when their frames come back.
    std::unique_ptr<plume::RenderQueryPool> pool = device_->CreateQueryPool(
        coverage_wanted_, "edram coverage checks", plume::RenderQueryType::OCCLUSION);
    if (pool) {
      // The slot's frame is complete: its old pool is no longer in use.
      coverage_pools_[slot] = std::move(pool);
      bool all = true;
      for (const auto& p : coverage_pools_) {
        all &= p && p->getCount() >= coverage_wanted_;
      }
      if (all) {
        coverage_capacity_ = coverage_wanted_;
      }
    }
  }
  list->resetQueryPool(coverage_pools_[slot].get(), 0, coverage_pools_[slot]->getCount());
}

void EdramTargets::Transition(plume::RenderCommandList* list, Target& target,
                              plume::RenderTextureLayout layout) {
  if (target.layout == layout || !target.texture) {
    return;
  }
  const plume::RenderTextureBarrier barrier(target.texture.get(), layout);
  // A target read as a texture is read by the resolve passes (graphics) and by
  // the resolve memory's depth encode (compute).
  const plume::RenderBarrierStages stages =
      layout == plume::RenderTextureLayout::SHADER_READ
          ? plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS |
                                       plume::RenderBarrierStage::COMPUTE)
          : plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS);
  list->barriers(stages, &barrier, 1);
  target.layout = layout;
  ++stats_.barriers;
}

plume::RenderFramebuffer* EdramTargets::Framebuffer(Target* const* colors, uint32_t color_count,
                                                    Target* depth) {
  FramebufferKey key;
  key.color_count = color_count;
  for (uint32_t i = 0; i < color_count && i < 4; ++i) {
    key.targets[i] = colors[i];
    key.generations[i] = colors[i] ? colors[i]->generation : 0;
  }
  key.targets[4] = depth;
  key.generations[4] = depth ? depth->generation : 0;
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return it->second.get();
  }
  const plume::RenderTexture* attachments[4] = {};
  for (uint32_t i = 0; i < color_count && i < 4; ++i) {
    attachments[i] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc(attachments, color_count,
                                    depth ? depth->texture.get() : nullptr);
  std::unique_ptr<plume::RenderFramebuffer> framebuffer =
      device_->CreateFramebuffer(desc, "edram framebuffer");
  if (!framebuffer) {
    return nullptr;
  }
  ++stats_.framebuffers_created;
  plume::RenderFramebuffer* raw = framebuffer.get();
  framebuffers_.emplace(key, std::move(framebuffer));
  return raw;
}

void EdramTargets::RetireFramebuffersOf(const Target* target) {
  ++change_serial_;
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    bool names = false;
    for (const Target* t : it->first.targets) {
      names |= t == target;
    }
    if (names) {
      frames_->Retire(std::move(it->second));
      it = framebuffers_.erase(it);
    } else {
      ++it;
    }
  }
}

TrimResult EdramTargets::Trim(uint64_t idle_frames, uint64_t bytes, uint32_t max_items) {
  TrimResult result;
  if (aliasing_ || tiling_) {
    return result;
  }
  std::vector<Target*> idle;
  for (const auto& [key, target] : targets_) {
    if (target->texture && target->last_frame + idle_frames < frame_) {
      idle.push_back(target.get());
    }
  }
  std::sort(idle.begin(), idle.end(),
            [](const Target* a, const Target* b) { return a->last_frame < b->last_frame; });
  for (Target* t : idle) {
    if (result.bytes >= bytes || result.items >= max_items) {
      break;
    }
    result.bytes += TextureBytes(t->scale.Edge(t->width), t->scale.Edge(t->height), 1, t->format,
                                 t->host_samples);
    ++result.items;
    // The framebuffers that name it, the shader views and their slots, then the
    // texture, each after the lists that still use them (FrameRing serials).
    RetireFramebuffersOf(t);
    ReleaseViews(*t);
    frames_->Retire(std::move(t->texture));
    t->height = 0;
    t->layout = plume::RenderTextureLayout::UNKNOWN;
    ++t->generation;
    ++stats_.trimmed;
  }
  if (result.items) {
    device_->NoteReleased(result.bytes);
  }
  return result;
}

uint64_t EdramTargets::HeldBytes() const {
  uint64_t bytes = 0;
  for (const auto& [key, target] : targets_) {
    if (target->texture) {
      bytes += TextureBytes(target->scale.Edge(target->width), target->scale.Edge(target->height),
                            1, target->format, target->host_samples);
    }
  }
  return bytes;
}

std::string EdramTargets::TakeSummary() {
  uint64_t bytes = 0;
  for (const auto& [key, target] : targets_) {
    if (!target->texture) {
      continue;  // released for want of video memory (Trim)
    }
    bytes += uint64_t(target->scale.Edge(target->width)) * target->scale.Edge(target->height) *
             BytesPerPixel(target->format) * target->host_samples;
  }
  std::string text = std::format(
      "edram: {} targets ({:.1f} MiB), {} framebuffers | created {}, grown {} ({:.1f} MiB "
      "copied), initialized {}, framebuffers made {}, {} barriers",
      targets_.size(), double(bytes) / 1048576.0, framebuffers_.size(), stats_.created,
      stats_.grown, double(stats_.grown_bytes) / 1048576.0, stats_.cleared,
      stats_.framebuffers_created, stats_.barriers);
  if (stats_.trimmed || stats_.remade) {
    text += std::format(" | released (video memory short, or idle for a minute) {}, made again {}",
                        stats_.trimmed, stats_.remade);
  }
  if (!resolution_scale_.unit()) {
    size_t scaled = 0;
    for (const auto& [key, target] : targets_) {
      scaled += !target->scale.unit();
    }
    text += std::format(" | scale {}x: {} of {} targets images of the screen (made now {} scaled, {} "
                        "at the guest's size), {} classifications without a declared size, {} uses "
                        "at a scale other than their target's",
                        resolution_scale_, scaled, targets_.size(), stats_.made_scaled,
                        stats_.made_unscaled, stats_.declared_unknown, stats_.scale_conflicts);
  }
  if (scene_decided_ && scene_scale_ != resolution_scale_) {
    text += std::format(" | the 3D scene at {}x ({}x{}) under images of the screen at {}x",
                        scene_scale_, scene_scale_.Edge(kAssumedScreenWidth),
                        scene_scale_.Edge(kAssumedScreenHeight), resolution_scale_);
  }
  if (stats_.rescaled != 0) {
    text += std::format(" | scene targets made again at another scale (a live upscaler change) {}",
                        stats_.rescaled);
  }
  if (msaa_level_ > 2) {
    text += std::format(" | host MSAA {}x: {} targets made with it", msaa_level_,
                        stats_.made_host_msaa);
  }
  if (aliasing_) {
    text += std::format(
        " | ownership: {} aspect-tiles taken over ({} draws unchecked), inherited {} "
        "aspect-tiles in {} writes and {} in {} reads ({} pairs so far), {} walks; tiled "
        "sections {}, {} accesses kept, {} noted per tile",
        stats_.tiles_taken, stats_.takes_unchecked, stats_.depend_tiles, stats_.depend_events,
        stats_.read_tiles, stats_.read_events, reported_pairs_.size(), stats_.walks,
        stats_.tiled_sections, stats_.tiled_kept, stats_.tiled_noted);
    text += std::format(
        "; reach: {} aspect-tiles marked, {} taken back, {} marked read, {} inherited resolves, "
        "readers {} fetches {} decodes {} locks",
        stats_.tainted, stats_.untainted, stats_.tainted_reads, stats_.inherited_resolves,
        stats_.reach[size_t(Reach::kFetch)], stats_.reach[size_t(Reach::kDecode)],
        stats_.reach[size_t(Reach::kLock)]);
  }
  if (coverage_) {
    text += std::format(
        "; coverage checks {} ({} full, {} partial, {} of them over another identity's tiles, "
        "{} overlapping, {} not recorded, {} candidates without a check)",
        stats_.checks, stats_.checks_full, stats_.checks_partial, stats_.checks_partial_taken,
        stats_.checks_over, stats_.checks_unrecorded, stats_.checks_wanting);
  }
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
