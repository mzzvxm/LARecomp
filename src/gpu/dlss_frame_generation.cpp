// MCLA GPU renderer -- NVIDIA DLSS frame generation. See dlss_frame_generation.h.

#include "dlss_frame_generation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "frame_layers.h"
#include "gpu.h"
#include "host_passes.h"
#include "host_shaders.h"
#include "latency.h"
#include "pass_timing.h"
#include "present.h"
#include "swap_chain.h"
#include "temporal.h"
#include "temporal_dlss.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_frame_generation_multiplier, 2, "MCLA/GPU",
    "How many frames DLSS frame generation shows for each one the game renders (mcla_gpu_frame_"
    "generation = dlss): 2 makes one in between, 3 two, 4 three. 3 and 4 need an RTX 50 series card "
    "(the plugin says how many the card takes; a higher number than it takes is lowered). The frame "
    "cap counts rendered frames (mcla_gpu_frame_generation_fps_limit). Live.")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(std::string, mcla_gpu_frame_generation);  // frame_generation.cpp
REXCVAR_DECLARE(int32_t, mcla_gpu_frame_generation_fps_limit);  // frame_generation.cpp
REXCVAR_DECLARE(bool, mcla_gpu_vsync);                    // renderer.cpp
REXCVAR_DECLARE(int32_t, mcla_gpu_hdr_layers_view);       // hdr_scene.cpp
REXCVAR_DECLARE(bool, mcla_gpu_hdr_test_pattern);         // hdr.cpp

namespace mcla::gpu {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr const char* kPrefix = "[gpu] dlss frame generation:";

// What the pause menu, the frame limiter and the latency reducer read, from any thread.
constexpr uint32_t kBitProbed = 1u << 0;
constexpr uint32_t kBitListed = 1u << 1;
constexpr uint32_t kBitSupported = 1u << 2;
constexpr uint32_t kBitAfterRestart = 1u << 3;
constexpr uint32_t kBitFailed = 1u << 4;
constexpr uint32_t kBitRunning = 1u << 5;
constexpr uint32_t kBitMultiplierChoice = 1u << 6;
std::atomic<uint32_t> g_bits{0};
std::atomic<uint32_t> g_max_multiplier{2};
std::atomic<uint32_t> g_multiplier{2};
std::atomic<uint32_t> g_refresh_hz{0};
std::mutex g_why_mutex;
std::string g_why;

// HALF DISPLAY: the floor of 97% of the display's rate over the multiplier, on a display of 100 Hz or
// more (a rendered frame rate that fills the display with the generated frames).
uint32_t HalfDisplayCap(uint32_t hz, uint32_t multiplier) {
  return hz >= 100 && multiplier >= 2
             ? uint32_t(std::floor(0.97 * double(hz) / double(multiplier)))
             : 0u;
}

// The refresh rate of the display the window is on, from its current mode (0 when it cannot be told).
uint32_t ReadRefresh(plume::RenderWindow window) {
  const HWND hwnd = window;
  const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    return 0;
  }
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    return 0;
  }
  return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0u;
}

struct UiPush {  // hlsl/dlssg_ui_ps.hlsl
  uint32_t ui;
  uint32_t ramp;
  uint32_t src_width;
  uint32_t src_height;
  float origin_x;
  float origin_y;
  float scale_x;
  float scale_y;
  float hdr_white;
  float hdr_peak;
  float hdr_knee;
  uint32_t hdr_flags;
};
static_assert(sizeof(UiPush) <= HostPasses::kPushConstantBytes);

constexpr plume::RenderFormat kUiFormat = plume::RenderFormat::R8G8B8A8_UNORM;

}  // namespace

// ---------------------------------------------------------------------------
// what the rest of the app asks (gpu.h)
// ---------------------------------------------------------------------------

DlssFrameGenerationAvailability GetDlssFrameGenerationAvailability() {
  const uint32_t bits = g_bits.load(std::memory_order_acquire);
  DlssFrameGenerationAvailability a;
  a.probed = (bits & kBitProbed) != 0;
  a.listed = (bits & kBitListed) != 0;
  a.supported = (bits & kBitSupported) != 0;
  a.after_restart = (bits & kBitAfterRestart) != 0;
  a.failed = (bits & kBitFailed) != 0;
  a.running = (bits & kBitRunning) != 0;
  a.multiplier_choice = (bits & kBitMultiplierChoice) != 0;
  a.max_multiplier = g_max_multiplier.load(std::memory_order_relaxed);
  a.refresh_hz = g_refresh_hz.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(g_why_mutex);
    a.why = g_why;
  }
  return a;
}

bool DlssFrameGenerationRunning() {
  return (g_bits.load(std::memory_order_acquire) & kBitRunning) != 0;
}

uint32_t DlssFrameGenerationMultiplier() {
  return g_multiplier.load(std::memory_order_relaxed);
}

int32_t DlssFrameGenerationFpsLimit(int32_t player_limit) {
  if (!DlssFrameGenerationRunning()) {
    return player_limit;
  }
  const int32_t cap = REXCVAR_GET(mcla_gpu_frame_generation_fps_limit);
  if (cap >= 0) {
    return cap;
  }
  // HALF DISPLAY: where the display is under 100 Hz, or unknown, the player's own cap applies.
  const uint32_t half = HalfDisplayCap(g_refresh_hz.load(std::memory_order_relaxed),
                                       g_multiplier.load(std::memory_order_relaxed));
  return half != 0 ? int32_t(half) : player_limit;
}

// ---------------------------------------------------------------------------
// life
// ---------------------------------------------------------------------------

DlssFrameGeneration::DlssFrameGeneration() = default;
DlssFrameGeneration::~DlssFrameGeneration() = default;

void DlssFrameGeneration::Initialize(Device& device, FrameRing& frames, HostPasses& passes,
                                     PassTiming& pass_timing, FrameLayers& layers,
                                     TemporalSystem& temporal, PresentPass& present,
                                     SwapChain& swap_chain) {
  device_ = &device;
  frames_ = &frames;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  layers_ = &layers;
  temporal_ = &temporal;
  present_ = &present;
  swap_chain_ = &swap_chain;
  vulkan_ = ActiveBackend() == Backend::kVulkan;
  gpu_name_ = device.description().name;
  vendor_ = uint32_t(device.description().vendor);
  files_ok_ = dlss::FrameGenerationFilesPresent(&files_missing_);
  initialized_ = true;
  if (vendor_ == kVendorNvidia && dlss::NameLooksLikeFrameGeneration(gpu_name_)) {
    if (files_ok_) {
      REXLOG_INFO("{} '{}' is an RTX 40 or 50 series card and the files are there: DLSS frame "
                  "generation is listed; Streamline is asked whether it runs when it has started",
                  kPrefix, gpu_name_);
    } else {
      REXLOG_INFO("{} not offered: {} (run the game's setup again to get them)", kPrefix,
                  files_missing_);
    }
  } else {
    REXLOG_INFO("{} not offered: '{}' is not an NVIDIA RTX 40 or 50 series card", kPrefix, gpu_name_);
  }
  Publish();
}

void DlssFrameGeneration::Shutdown(SwapChain& swap_chain) {
  if (!initialized_) {
    return;
  }
  if (active_ || plugin_loaded_) {
    // The plugin owns the swap chain: it is told to stop generating, the chain is destroyed (the GPU
    // is idle), then the plugin goes. Streamline shuts down after this (temporal_dlss.h).
    if (options_known_ && applied_.on) {
      dlss::FrameGenerationOptions off;
      off.on = false;
      std::string why;
      bool busy = false;
      dlss::SetFrameGenerationOptions(off, &why, &busy);
    }
    ReleaseImages("shutdown");
    swap_chain.Shutdown();
    dlss::NoteSwapChainProxied(false);
    dlss::ShutdownFrameGeneration();
    plugin_loaded_ = false;
  }
  ReleaseImages("shutdown");
  ui_pipeline_.reset();
  ui_shader_.reset();
  active_ = false;
  initialized_ = false;
  g_bits.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

bool DlssFrameGeneration::SettingIsDlss() const {
  return REXCVAR_GET(mcla_gpu_frame_generation) == "dlss";
}

bool DlssFrameGeneration::Candidate() const {
#if !defined(LARECOMP_PLUME_STREAMLINE)
  // Vulkan needs Plume's creation hooks for Streamline (the instance and the device are made through
  // its interposer): without them there is no DLSS frame generation there, and the row is hidden.
  if (vulkan_) {
    return false;
  }
#endif
  return initialized_ && vendor_ == kVendorNvidia && files_ok_ &&
         dlss::NameLooksLikeFrameGeneration(gpu_name_);
}

bool DlssFrameGeneration::WantsVsync() const {
  // The plugin supports vertical sync on D3D12 only (its guide's section 22); the player's own
  // setting applies there, where it says so.
  return !vulkan_ && REXCVAR_GET(mcla_gpu_vsync) && dlss::GetFrameGenerationFacts().vsync_supported;
}

void DlssFrameGeneration::ReadDisplayRefresh() {
  const auto now = Clock::now();
  if (refresh_read_ != Clock::time_point{} && now - refresh_read_ < std::chrono::seconds(2)) {
    return;
  }
  refresh_read_ = now;
  if (!swap_chain_) {
    return;
  }
  const uint32_t hz = ReadRefresh(swap_chain_->window());
  if (hz != refresh_hz_) {
    refresh_hz_ = hz;
    g_refresh_hz.store(hz, std::memory_order_relaxed);
  }
}

uint32_t DlssFrameGeneration::Multiplier() const {
  const dlss::FrameGenerationFacts facts = dlss::GetFrameGenerationFacts();
  uint32_t most = kMaxMultiplier;
  if (facts.queried && facts.supported) {
    most = std::min<uint32_t>(kMaxMultiplier, std::max<uint32_t>(facts.max_frames_generated, 1u) + 1u);
  } else if (!dlss::NameLooksLikeMultiFrameGeneration(gpu_name_)) {
    most = 2;
  }
  const int32_t asked = REXCVAR_GET(mcla_gpu_frame_generation_multiplier);
  return std::clamp<uint32_t>(uint32_t(std::max(asked, 2)), 2u, most);
}

void DlssFrameGeneration::Publish() {
  const dlss::FrameGenerationFacts facts = dlss::GetFrameGenerationFacts();
  const bool candidate = Candidate();
  const bool setting = SettingIsDlss();
  uint32_t bits = 0;
  std::string why;
  bool after_restart = false;
  if (!candidate) {
    bits |= kBitProbed;
    why = !initialized_ ? std::string()
          : (vendor_ != kVendorNvidia || !dlss::NameLooksLikeFrameGeneration(gpu_name_))
              ? std::string("DLSS frame generation needs an NVIDIA RTX 40 or 50 series card")
          : !files_ok_ ? "these files are not next to the game's exe: " + files_missing_
                       : std::string("this build's Plume has no creation hooks for Streamline on "
                                     "Vulkan, which DLSS frame generation needs there");
  } else {
    // Vulkan: the device is made through Streamline only when frame generation or DLSS is the saved
    // choice at the start (temporal_dlss.h), and the plugins are asked for only with frame
    // generation saved: otherwise it runs from the next start.
    if (vulkan_ && (dlss::VulkanInstanceProcAddr() == nullptr ||
                    (facts.queried && !facts.asked))) {
      after_restart = true;
      bits |= kBitProbed | kBitListed | kBitAfterRestart;
      why = "on Vulkan DLSS frame generation starts with the game";
    } else if (!facts.queried) {
      bits |= kBitListed;  // Streamline is still starting: listed, not final
    } else if (!facts.supported) {
      // Not offered, unless it was chosen (the row then says so).
      bits |= kBitProbed | (setting ? kBitListed : 0u);
      why = facts.why;
    } else {
      bits |= kBitProbed | kBitListed | kBitSupported;
    }
  }
  if (failed_) {
    bits |= kBitFailed | kBitListed;
    if (why.empty()) {
      why = "it failed earlier in this session";
    }
  }
  if (active_) {
    bits |= kBitRunning;
  }
  uint32_t most = 2;
  if (facts.queried && facts.supported) {
    most = std::min<uint32_t>(kMaxMultiplier, std::max<uint32_t>(facts.max_frames_generated, 1u) + 1u);
  } else if (candidate && dlss::NameLooksLikeMultiFrameGeneration(gpu_name_)) {
    most = kMaxMultiplier;
  }
  if (most > 2) {
    bits |= kBitMultiplierChoice;
  }
  g_max_multiplier.store(most, std::memory_order_relaxed);
  g_multiplier.store(Multiplier(), std::memory_order_relaxed);
  min_size_ = facts.min_size;
  const uint32_t before = g_bits.exchange(bits, std::memory_order_acq_rel);
  if (!why.empty() || before != bits) {
    std::lock_guard lock(g_why_mutex);
    g_why = why;
  }
}

DlssFrameGeneration::Change DlssFrameGeneration::Poll() {
  if (!initialized_) {
    return Change::kNone;
  }
  ReadDisplayRefresh();
  if (unload_pending_) {
    std::string why;
    if (dlss::LoadFrameGeneration(false, &why) == dlss::LoadResult::kDone) {
      unload_pending_ = false;
      plugin_loaded_ = false;
    }
  }
  const dlss::FrameGenerationFacts facts = dlss::GetFrameGenerationFacts();
  const bool setting = SettingIsDlss();
  const bool can_run = Candidate() && facts.queried && facts.supported && !failed_ &&
                       dlss::reflex::Available() &&
                       !(vulkan_ && (dlss::VulkanInstanceProcAddr() == nullptr || !facts.asked));
  wanted_ = setting && can_run && Clock::now() >= settle_until_ && !unload_pending_;
  Publish();
  if (setting && !can_run && !logged_unavailable_ && (facts.queried || failed_)) {
    logged_unavailable_ = true;
    REXLOG_INFO("{} is chosen but cannot run here: {}", kPrefix,
                failed_ ? std::string("it failed earlier in this session")
                        : (facts.why.empty() ? std::string("Streamline does not run it") : facts.why));
  }
  // The groundwork follows the setting while it can run: the temporal system keeps the depth, the
  // vectors and the camera, the frame layers keep the hudless colour and the UI.
  const bool need = (setting && can_run) || active_;
  temporal_->RequestVectorsForDlssFrameGeneration(need);
  layers_->Request(FrameLayers::kRequesterDlssFrameGeneration, need);
  if (wanted_ && !active_) {
    return Change::kOn;
  }
  if (active_ && (!setting || !can_run)) {
    return Change::kOff;
  }
  return Change::kNone;
}

DlssFrameGeneration::Prepared DlssFrameGeneration::PrepareSwitchOn() {
  std::string why;
  const dlss::LoadResult result = dlss::LoadFrameGeneration(true, &why);
  if (result == dlss::LoadResult::kBusy) {
    return Prepared::kWait;
  }
  if (result == dlss::LoadResult::kFailed) {
    failed_ = true;
    REXLOG_ERROR("{} the plugin could not be loaded: {}; it stays off for this session", kPrefix, why);
    Publish();
    return Prepared::kFailed;
  }
  plugin_loaded_ = true;
  // The swap chain made from now on is the plugin's own: no wrapper of ours goes over it.
  dlss::NoteSwapChainProxied(true);
  return Prepared::kReady;
}

void DlssFrameGeneration::BeforePlainChain() {
  dlss::NoteSwapChainProxied(false);
  // The plugin goes only once its chain is gone (the guide's section 18). It may be inside a feature's
  // creation (seconds, the worker holds Streamline then); the chain is made plain either way, and the
  // unload is asked again by Poll.
  std::string why;
  for (int attempt = 0; attempt < 300; ++attempt) {
    const dlss::LoadResult result = dlss::LoadFrameGeneration(false, &why);
    if (result == dlss::LoadResult::kDone) {
      plugin_loaded_ = false;
      return;
    }
    if (result == dlss::LoadResult::kFailed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  unload_pending_ = true;
  REXLOG_WARN("{} the plugin could not be unloaded at once ({}); it is asked again", kPrefix,
              why.empty() ? std::string("Streamline is busy") : why);
}

void DlssFrameGeneration::CompleteSwitch(Change change, bool ok, bool resize) {
  options_known_ = false;
  applied_ = Options{};
  frames_on_ = 0;
  status_failures_ = 0;
  last_frame_ = ~uint64_t(0);
  idle_guest_frames_ = 0;
  if (change == Change::kOn) {
    if (!ok) {
      failed_ = true;
      dlss::NoteSwapChainProxied(false);
      BeforePlainChain();
      REXLOG_ERROR("{} the swap chain could not be made through Streamline's proxy; it stays off for "
                   "this session",
                   kPrefix);
      Publish();
      return;
    }
    if (!latency::UseStreamline(true)) {
      // Frame generation cannot run without Reflex through Streamline: off at the next frame.
      failed_ = true;
      REXLOG_ERROR("{} Reflex through Streamline cannot be used; it stays off for this session",
                   kPrefix);
    }
    active_ = true;
    Publish();
    REXLOG_INFO("{} on ({}x: the swap chain is Streamline's, Reflex goes through it, the display runs "
                "at {} Hz, the frame limit counts rendered frames)",
                kPrefix, Multiplier(), refresh_hz_);
    return;
  }
  // Off: the swap chain is plain again (or could not be made at all).
  active_ = false;
  latency::UseStreamline(false);
  if (swap_chain_) {
    // The device's own reducer is current again: it is told the chain it now has.
    latency::SwapChainChange change_scope;
    change_scope.Replaced(*swap_chain_);
  }
  ReleaseImages("frame generation is off");
  if (resize) {
    settle_until_ = Clock::now() + kSettle;
  }
  Publish();
  REXLOG_INFO("{} off{}; the swap chain is plain again", kPrefix,
              ok ? (resize ? " for a change of the window (back on once it has been still)" : "")
                 : " (the switch had trouble: see above)");
}

void DlssFrameGeneration::OnSwapChainChanged() {
  // A new chain in another format or size: the images follow it (made again at the next frame) and
  // so does the UI pass's pipeline (its colour step is the back buffer's).
  ReleaseImages("the swap chain changed");
  ui_pipeline_.reset();
  ui_shader_.reset();
  options_known_ = false;  // the plugin's sizes and buffer count are told again
}

void DlssFrameGeneration::Fail(const std::string& why) {
  if (!failed_) {
    REXLOG_ERROR("{} {}; it is switched off for the rest of this session", kPrefix, why);
  }
  failed_ = true;
  Publish();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool DlssFrameGeneration::MakePasses(bool hdr) {
  if (ui_pipeline_ && ui_hdr_ == hdr) {
    return true;
  }
  const HostShaderBlob blob = DlssFrameGenerationUiPixelShader(hdr);
  ui_pipeline_.reset();
  ui_shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (!ui_shader_) {
    return false;
  }
  ui_pipeline_ = passes_->CreatePipeline(*ui_shader_, kUiFormat, "dlss frame generation ui");
  ui_hdr_ = hdr;
  return ui_pipeline_ != nullptr;
}

bool DlssFrameGeneration::EnsureImages(uint32_t width, uint32_t height, plume::RenderFormat format) {
  return passes_->EnsureImage(hudless_, width, height, format, "dlss frame generation hudless") &&
         passes_->EnsureImage(ui_, width, height, kUiFormat, "dlss frame generation ui");
}

void DlssFrameGeneration::ReleaseImages(const char* why) {
  const bool had = hudless_.texture || ui_.texture;
  passes_->ReleaseImage(hudless_);
  passes_->ReleaseImage(ui_);
  if (had) {
    REXLOG_INFO("{} the window-size images are released ({})", kPrefix, why);
  }
}

bool DlssFrameGeneration::ApplyOptions(bool on, const FrameArgs& args, uint32_t render_w,
                                       uint32_t render_h) {
  if (!on && (!options_known_ || !applied_.on)) {
    return true;  // nothing was told on: nothing to take back
  }
  Options want;
  want.on = on;
  if (on) {
    want.frames = Multiplier() - 1;
    want.back[0] = args.window_width;
    want.back[1] = args.window_height;
    want.render[0] = render_w;
    want.render[1] = render_h;
    want.buffers = args.back_buffers;
  } else {
    want = applied_;
    want.on = false;
  }
  if (options_known_ && want == applied_) {
    return true;
  }
  dlss::FrameGenerationOptions options;
  options.on = want.on;
  options.frames_to_generate = std::max(want.frames, 1u);
  options.color_size[0] = want.back[0];
  options.color_size[1] = want.back[1];
  options.mvec_depth_size[0] = want.render[0];
  options.mvec_depth_size[1] = want.render[1];
  options.back_buffers = want.buffers;
  std::string why;
  bool busy = false;
  if (!dlss::SetFrameGenerationOptions(options, &why, &busy)) {
    if (!busy) {
      Fail(why);
    }
    return false;
  }
  if (on && !applied_.on) {
    frames_on_ = 0;
    last_frame_ = ~uint64_t(0);  // the first generated frame starts the history afresh
    REXLOG_INFO("{} generating: {}x, back buffer {}x{}, depth and vectors {}x{}", kPrefix,
                want.frames + 1, want.back[0], want.back[1], want.render[0], want.render[1]);
  } else if (!on && applied_.on) {
    REXLOG_INFO("{} paused (not a game scene): the plugin keeps its memory", kPrefix);
  }
  applied_ = want;
  options_known_ = true;
  ++stats_.option_changes;
  return true;
}

bool DlssFrameGeneration::NotGenerated(uint64_t& counter, const FrameArgs& args) {
  ++counter;
  if (++idle_guest_frames_ >= kReleaseAfterFrames && (hudless_.texture || ui_.texture)) {
    ReleaseImages(std::format("nothing generated for {} guest frames", kReleaseAfterFrames).c_str());
  }
  // Generation is told off (and its memory kept) in a frame that is not a rendered game scene.
  ApplyOptions(false, args, 0, 0);
  return false;
}

bool DlssFrameGeneration::Record(plume::RenderCommandList* list, const FrameArgs& args) {
  ++stats_.presents;
  if (!args.guest_frame) {
    return NotGenerated(stats_.not_idle, args);  // an idle present repeats the last frame
  }
  // The HDR debug views and the test pattern show something else than the frame.
  if (REXCVAR_GET(mcla_gpu_hdr_layers_view) != 0 || REXCVAR_GET(mcla_gpu_hdr_test_pattern)) {
    return NotGenerated(stats_.not_scene, args);
  }
  // The frame's id: what the constants, the tags and Reflex's markers share. A frame without one is
  // not marked (the logos, a loading screen); and Reflex through Streamline must have run for a few
  // frames before the plugin is told to generate (its status says so otherwise).
  const uint64_t id = latency::RenderFrameId();
  if (id == 0) {
    return NotGenerated(stats_.not_unmarked, args);
  }
  if (!latency::StreamlineRunning()) {
    return NotGenerated(stats_.not_reflex, args);
  }
  const TemporalSystem::FrameState& fr = temporal_->frame();
  const TemporalMotion::Kept kept = temporal_->motion().kept();
  if (!fr.anchor_ok || !fr.streamline_camera.valid || !kept.valid || kept.frame != fr.frame ||
      !kept.depth || !kept.motion || !kept.depth->texture || !kept.motion->texture) {
    return NotGenerated(stats_.not_scene, args);  // the pause menu, the map, a menu, a loading screen
  }
  const FrameLayers::Frame layer = layers_->presentable();
  const uint32_t src_w = args.front_width;
  const uint32_t src_h = args.front_height;
  // The picture is exactly what the layers cover: the hudless image is shown whole where the front
  // buffer is (PresentPass::PrepareImage fits the whole image), so a layer region larger than the
  // front buffer cannot be used as it is.
  if (!layer.valid || !layer.IsPictureOf(args.front) || !layer.hudless || !layer.ui || src_w == 0 ||
      src_h == 0 || layer.width != src_w || layer.height != src_h) {
    return NotGenerated(stats_.not_layers, args);
  }
  const HostImage* float_scene = layers_->FloatSceneOf(layer, src_w, src_h);
  const HostImage* scene = float_scene ? float_scene : layer.hudless;
  if (!scene->texture || scene->slot == kInvalidBindlessSlot || !layer.ui->texture ||
      layer.ui->slot == kInvalidBindlessSlot) {
    return NotGenerated(stats_.not_layers, args);
  }
  if (min_size_ != 0 && (args.window_width < min_size_ || args.window_height < min_size_)) {
    return NotGenerated(stats_.not_small, args);
  }
  const uint32_t window_w = args.window_width;
  const uint32_t window_h = args.window_height;
  if (!EnsureImages(window_w, window_h, swap_chain_->format()) || !MakePasses(args.hdr) ||
      !present_->ramp_ready()) {
    return NotGenerated(stats_.not_failed, args);
  }
  idle_guest_frames_ = 0;

  // 1. The picture without the UI, as the back buffer would have it: the present pass again, over the
  // frame layers' hudless image (or the float scene), into the image of the swap chain's format.
  namespace Stage = plume::RenderBarrierStage;
  pass_timing_->Mark(list, "dlss frame generation");
  present_->PrepareImage(list, window_w, window_h, *scene, float_scene != nullptr);
  if (!present_->source_ready()) {
    return NotGenerated(stats_.not_failed, args);
  }
  list->barriers(Stage::GRAPHICS,
                 plume::RenderTextureBarrier(hudless_.texture.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(hudless_.framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  present_->Draw(list, window_w, window_h);
  list->setFramebuffer(nullptr);

  // 2. The UI layer laid into the window as the picture is, in the back buffer's colour terms.
  {
    uint32_t fit_x = 0, fit_y = 0, fit_w = 0, fit_h = 0;
    present_->LastFit(&fit_x, &fit_y, &fit_w, &fit_h);
    UiPush push{};
    push.ui = layer.ui->slot;
    push.ramp = present_->ramp_slot();
    push.src_width = src_w;
    push.src_height = src_h;
    push.origin_x = float(fit_x);
    push.origin_y = float(fit_y);
    push.scale_x = fit_w ? float(src_w) / float(fit_w) : 1.0f;
    push.scale_y = fit_h ? float(src_h) / float(fit_h) : 1.0f;
    if (args.hdr) {
      const HdrLevels& levels = present_->hdr_levels();
      push.hdr_white = levels.white;
      push.hdr_peak = levels.peak;
      push.hdr_knee = levels.knee;
      push.hdr_flags = levels.flags & 1u;  // the transfer; the rest is for the present's own passes
    }
    passes_->DrawInto(list, ui_, ui_pipeline_.get(), &push, sizeof(push), /*clear=*/true);
  }

  // 3. The four tagged images in the compute read state, which is the state their tags give, and
  // where they stay until the present.
  plume::RenderTextureBarrier to_read[4] = {
      plume::RenderTextureBarrier(hudless_.texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(ui_.texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::COMPUTE, to_read, 4);
  hudless_.layout = plume::RenderTextureLayout::SHADER_READ;
  ui_.layout = plume::RenderTextureLayout::SHADER_READ;

  // 4. The frame's constants and tags (Streamline's frame index is the latency id).
  dlss::FrameGenerationInputs in;
  in.frame_index = uint32_t(id);
  in.render_size[0] = kept.depth->width;
  in.render_size[1] = kept.depth->height;
  in.back_size[0] = window_w;
  in.back_size[1] = window_h;
  in.depth = {kept.depth->texture.get(), kept.depth->width, kept.depth->height, kept.depth->format,
              kept.depth->flags};
  in.motion = {kept.motion->texture.get(), kept.motion->width, kept.motion->height,
               kept.motion->format, kept.motion->flags};
  in.hudless = {hudless_.texture.get(), window_w, window_h, hudless_.format, hudless_.flags};
  in.ui = {ui_.texture.get(), window_w, window_h, ui_.format, ui_.flags};
  in.jitter[0] = fr.jitter[0];
  in.jitter[1] = fr.jitter[1];
  in.reset = fr.reset != 0 || last_frame_ == ~uint64_t(0) || last_frame_ + 1 != fr.frame ||
             frames_on_ == 0;
  in.camera_near = float(fr.camera.near_plane);
  in.camera_far = float(fr.camera.far_plane);
  in.camera_fov_y = float(fr.camera.fov_y);
  in.camera_aspect = fr.streamline_camera.aspect;
  std::memcpy(in.view_to_clip, fr.streamline_camera.view_to_clip, sizeof(in.view_to_clip));
  std::memcpy(in.clip_to_view, fr.streamline_camera.clip_to_view, sizeof(in.clip_to_view));
  std::memcpy(in.clip_to_prev_clip, fr.streamline_camera.clip_to_prev_clip,
              sizeof(in.clip_to_prev_clip));
  std::memcpy(in.prev_clip_to_clip, fr.streamline_camera.prev_clip_to_clip,
              sizeof(in.prev_clip_to_clip));
  for (int i = 0; i < 3; ++i) {
    in.camera_position[i] = float(fr.camera.position[i]);
    in.camera_up[i] = float(fr.camera.up[i]);
    in.camera_right[i] = float(fr.camera.right[i]);
    in.camera_forward[i] = float(fr.camera.forward[i]);
  }
  std::string why;
  bool busy = false;
  if (!dlss::TagFrameGeneration(backend::NativeListHandle(list), in, &why, &busy)) {
    if (busy) {
      return NotGenerated(stats_.not_busy, args);
    }
    Fail(why);
    return NotGenerated(stats_.not_failed, args);
  }
  // 5. Generate: the options, when they changed (and the first time).
  if (!ApplyOptions(true, args, kept.depth->width, kept.depth->height)) {
    return NotGenerated(stats_.not_busy, args);
  }
  last_frame_ = fr.frame;
  ++frames_on_;
  ++stats_.generated;
  return true;
}

void DlssFrameGeneration::AfterPresent() {
  if (!active_ || ++frames_since_state_ < 60) {
    return;
  }
  frames_since_state_ = 0;
  dlss::FrameGenerationState state;
  std::string why;
  bool busy = false;
  if (!dlss::ReadFrameGenerationState(&state, /*request_vram_estimate=*/false, &why, &busy)) {
    if (!busy) {
      Fail(why);
    }
    return;
  }
  NoteState();
  // Streamline's own verdict on the inputs: anything but ok means it did not generate. Reflex missing is
  // a start-up state (it needs a few frames with the markers) and may come and go for a second; the
  // rest is the integration's.
  if (!applied_.on) {
    return;  // paused: the status is of the last frame generated
  }
  const bool bad = state.resolution_too_low || state.hdr_format_unsupported ||
                   state.constants_invalid || state.back_buffer_index_not_asked || state.reflex_missing;
  if (!bad) {
    status_failures_ = 0;
    return;
  }
  if (state.reflex_missing && ++status_failures_ < 5) {
    return;
  }
  std::string reasons;
  const auto add = [&reasons](bool flag, const char* text) {
    if (flag) {
      reasons += (reasons.empty() ? "" : ", ") + std::string(text);
    }
  };
  add(state.resolution_too_low, "the back buffer is under its smallest size");
  add(state.reflex_missing, "it did not see Reflex run with the frame markers");
  add(state.hdr_format_unsupported, "it does not take this HDR format");
  add(state.constants_invalid, "its constants were invalid");
  add(state.back_buffer_index_not_asked, "the swap chain's back buffer index was not asked for");
  Fail("Streamline's status says it does not generate: " + reasons);
}

void DlssFrameGeneration::NoteState() {}

uint64_t DlssFrameGeneration::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  add(hudless_);
  add(ui_);
  return bytes;
}

std::string DlssFrameGeneration::TakeSummary() {
  const Stats s = stats_;
  stats_ = Stats{};
  return std::format(
      "dlss frame generation: {} presents, {} generated ({}x), not generated: {} idle, {} not "
      "marked, {} reflex not running, {} not a game scene, {} layers unusable, {} streamline "
      "busy, {} too small, {} failed; {} option changes; the display runs at {} Hz",
      s.presents, s.generated, Multiplier(), s.not_idle, s.not_unmarked, s.not_reflex, s.not_scene,
      s.not_layers, s.not_busy, s.not_small, s.not_failed, s.option_changes, refresh_hz_);
}

}  // namespace mcla::gpu
