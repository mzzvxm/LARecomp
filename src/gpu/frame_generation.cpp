// MCLA GPU renderer -- FSR 3.1 frame generation. See frame_generation.h.
//
// The pacing follows the formulas of AMD's FidelityFX frame interpolation swap chain (MIT, FidelityFX
// SDK, framegeneration/include/ffx_framegeneration_api_types.h, "Tuning varianceFactor and
// safetyMarginInMs": the present interval is the average frame time less the variance times a factor
// (0.1) less a margin (0.1 ms)). The design of the pacer thread and the rules around it (the interval
// window, the stalled-pacer timeout, the cap, the context's release) are those of the MCLA fork's
// Vulkan frame generation, written by the same author (patches/rexglue-frame-generation.patch).

#include "frame_generation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

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
#include "swap_chain.h"
#include "temporal.h"
#include "texture_bytes.h"
#include "thread_load.h"
#include "vram_budget.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_frame_generation, "off", "MCLA/GPU",
    "Frame generation: off, or fsr31 (AMD FSR 3.1 frame generation, on any graphics card, with or "
    "without an upscaler). An interpolated frame is made between every two the game renders and "
    "shown half a frame before the real one, so a game that renders 60 frames a second shows 120. "
    "It does not make the game react faster: the real frames are shown one frame later and the "
    "latency reducer (NVIDIA Reflex, AMD Anti-Lag 2) is switched on while it runs, where the card "
    "has one. Only the 3D scene is generated; the menus, the pause menu, the map and the loading "
    "screens are shown as the game draws them, and the interface is drawn over each generated "
    "frame. The frame rate limit counts rendered frames while it runs "
    "(mcla_gpu_frame_generation_fps_limit). Wants a display of 100 Hz or more or a variable refresh "
    "display. Live: it is taken at a frame boundary and the picture may flicker once while the "
    "window's swap chain is made again. Only offered where the graphics card and the AMD files next "
    "to the game can run it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_frame_generation_fps_limit, -1, "MCLA/GPU",
    "The frame rate cap while frame generation runs, in frames the game RENDERS a second (twice as "
    "many are shown): -1 is HALF DISPLAY, the default, the floor of 97% of half of the display's "
    "refresh rate (116 at 240 Hz, 58 at 120 Hz), which fills the display; it is only offered on a "
    "display of 100 Hz or more, and where it is not the cap you chose (fps_limit) applies. 0 is no "
    "cap, and a number from 1 to 360 caps the rendered frames at it (60 shows 120). A cap above half "
    "the display's rate adds latency: the frames queue in front of the display. Live.")
    .range(-1, 360)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_frame_generation_tearing, false, "MCLA/GPU",
    "Frame generation presents with vertical sync (FIFO), so a frame is never torn and a variable "
    "refresh display follows the frames. On presents immediately instead, which tears. Read when "
    "frame generation is switched on.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(int32_t, mcla_gpu_hdr_layers_view);  // hdr_scene.cpp
REXCVAR_DECLARE(bool, mcla_gpu_hdr_test_pattern);     // hdr.cpp

namespace mcla::gpu {

namespace {

using Clock = std::chrono::steady_clock;

// What the pause menu, the frame limiter and the latency reducer read, from any thread.
constexpr uint32_t kBitProbed = 1u << 0;
constexpr uint32_t kBitAvailable = 1u << 1;
constexpr uint32_t kBitFailed = 1u << 2;
constexpr uint32_t kBitRunning = 1u << 3;
constexpr uint32_t kBitNoRoom = 1u << 4;
std::atomic<uint32_t> g_bits{0};
std::atomic<uint32_t> g_refresh_hz{0};
std::mutex g_why_mutex;
std::string g_why;

// HALF DISPLAY: the floor of 97% of half the refresh rate, on a display of 100 Hz or more.
uint32_t HalfDisplayCap(uint32_t hz) {
  return hz >= 100 ? uint32_t(std::floor(0.97 * double(hz) / 2.0)) : 0u;
}

// The refresh rate of the display the window is on, from its current mode (a variable refresh
// display reports its highest). 0 when it cannot be told.
uint32_t ReadDisplayRefresh(plume::RenderWindow window) {
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
  // 0 and 1 mean the hardware's default.
  return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0u;
}

struct PreparePush {  // hlsl/fg_prepare_ps.hlsl
  uint32_t scene;
  uint32_t flags;
  uint32_t src_width;
  uint32_t src_height;
  uint32_t out_width;
  uint32_t out_height;
};
struct ComposePush {  // hlsl/fg_compose_ps.hlsl
  uint32_t generated;
  uint32_t ui;
  uint32_t flags;
  uint32_t src_width;
  uint32_t src_height;
  uint32_t out_width;
  uint32_t out_height;
};
struct CopyPush {  // hlsl/fg_copy_ps.hlsl
  uint32_t source;
};
static_assert(sizeof(PreparePush) <= HostPasses::kPushConstantBytes);
static_assert(sizeof(ComposePush) <= HostPasses::kPushConstantBytes);
constexpr uint32_t kFlagFloatScene = 1u;

constexpr plume::RenderFormat kGenerationFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr plume::RenderTextureFlags kOutputFlags =
    plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

}  // namespace

// ---------------------------------------------------------------------------
// what the rest of the app asks (gpu.h)
// ---------------------------------------------------------------------------

FrameGenerationAvailability GetFrameGenerationAvailability() {
  const uint32_t bits = g_bits.load(std::memory_order_acquire);
  FrameGenerationAvailability a;
  a.probed = (bits & kBitProbed) != 0;
  a.available = (bits & kBitAvailable) != 0;
  a.failed = (bits & kBitFailed) != 0;
  a.no_room = (bits & kBitNoRoom) != 0;
  a.running = (bits & kBitRunning) != 0;
  a.refresh_hz = g_refresh_hz.load(std::memory_order_relaxed);
  a.half_display = HalfDisplayCap(a.refresh_hz);
  {
    std::lock_guard lock(g_why_mutex);
    a.why = g_why;
  }
  return a;
}

bool FrameGenerationRunning() {
  return (g_bits.load(std::memory_order_acquire) & kBitRunning) != 0;
}

int32_t FrameGenerationFpsLimit(int32_t player_limit) {
  if (!FrameGenerationRunning()) {
    return player_limit;
  }
  const int32_t cap = REXCVAR_GET(mcla_gpu_frame_generation_fps_limit);
  if (cap >= 0) {
    return cap;
  }
  // HALF DISPLAY: where the display is under 100 Hz, or unknown, the player's own cap applies (half of
  // a 60 or 75 Hz display would be under 50 rendered frames, which looks choppy between the generated
  // ones).
  const uint32_t half = HalfDisplayCap(g_refresh_hz.load(std::memory_order_relaxed));
  return half != 0 ? int32_t(half) : player_limit;
}

// ---------------------------------------------------------------------------
// life
// ---------------------------------------------------------------------------

FrameGeneration::FrameGeneration() = default;

FrameGeneration::~FrameGeneration() {
  StopPacer();
}

int64_t FrameGeneration::NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

void FrameGeneration::Initialize(Device& device, FrameRing& frames, HostPasses& passes,
                                 PassTiming& pass_timing, FrameLayers& layers,
                                 TemporalSystem& temporal, SwapChain& swap_chain,
                                 std::atomic<bool>* resize_flag) {
  device_ = &device;
  frames_ = &frames;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  layers_ = &layers;
  temporal_ = &temporal;
  swap_chain_ = &swap_chain;
  resize_flag_ = resize_flag;
  initialized_ = true;
#if defined(LARECOMP_HAS_FFX_FG)
  backend_ = MakeFfxFrameGeneration();
#endif
  if (backend_) {
    backend_->Probe(device, frames);
  } else {
    {
      std::lock_guard lock(g_why_mutex);
      g_why = "this build has no AMD FidelityFX frame generation";
    }
    g_bits.store(kBitProbed, std::memory_order_release);
    REXLOG_INFO("[gpu] frame generation: not offered, this build has no AMD FidelityFX frame "
                "generation");
  }
  UpdateDisplayRefresh();
}

void FrameGeneration::StopPacer() {
  std::thread thread;
  {
    std::unique_lock lock(mu_);
    if (!thread_.joinable()) {
      return;
    }
    stop_ = true;
    cv_.notify_all();
    // The thread drains what it was handed, then ends. A present blocked in the driver is the one
    // thing that could keep it: given a few seconds, then left behind (the renderer is never deleted).
    cv_.wait_for(lock, std::chrono::seconds(3), [this] { return !running_; });
    if (running_) {
      REXLOG_ERROR("[gpu] frame generation: the pacer thread did not stop in 3 s (a present or an "
                   "acquire is stuck in the driver); it is left running");
      thread_.detach();
      return;
    }
    thread = std::move(thread_);
  }
  thread.join();
  // The presents the thread made are still queued on the present queue: the swap chain may go (or be
  // made again) only after they were handed to the driver.
  DrainPresentQueue();
}

void FrameGeneration::Shutdown() {
  StopPacer();
  if (backend_) {
    backend_->Shutdown();
    backend_.reset();
  }
  for (HostImage* image : {&color_, &generated_, &composed_}) {
    *image = HostImage{};
  }
  for (Slot& slot : slots_) {
    slot.generated = HostImage{};
    slot.real = HostImage{};
    slot.marker.reset();
    slot.frame_done.reset();
  }
  prepare_pipeline_.reset();
  compose_pipeline_.reset();
  copy_pipeline_.reset();
  prepare_shader_.reset();
  compose_shader_.reset();
  copy_shader_.reset();
  g_bits.store(0, std::memory_order_release);
  ClearMemoryBlock();
  have_request_ = false;
  initialized_ = false;
}

void FrameGeneration::ReleaseQueue() {
  for (Lane& lane : lanes_) {
    lane = Lane{};
  }
  idle_list_.reset();
  idle_fence_.reset();
  present_queue_.reset();
}

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

bool FrameGeneration::WantedBySetting() const {
  return REXCVAR_GET(mcla_gpu_frame_generation) == "fsr31";
}

bool FrameGeneration::WantsVsync() const {
  return !REXCVAR_GET(mcla_gpu_frame_generation_tearing);
}

void FrameGeneration::UpdateDisplayRefresh() {
  const auto now = Clock::now();
  if (refresh_read_ != Clock::time_point{} && now - refresh_read_ < std::chrono::seconds(2)) {
    return;
  }
  refresh_read_ = now;
  if (!swap_chain_) {
    return;
  }
  const uint32_t hz = ReadDisplayRefresh(swap_chain_->window());
  if (hz != refresh_hz_) {
    refresh_hz_ = hz;
    g_refresh_hz.store(hz, std::memory_order_relaxed);
    REXLOG_INFO("[gpu] frame generation: the display the window is on runs at {} Hz{}", hz,
                HalfDisplayCap(hz) != 0
                    ? std::format("; HALF DISPLAY is {} rendered frames a second", HalfDisplayCap(hz))
                    : std::string("; HALF DISPLAY is not offered under 100 Hz (or when it is "
                                  "unknown), fps_limit applies"));
  }
}

void FrameGeneration::Publish(bool setting) {
  uint32_t bits = 0;
  std::string why;
  if (!backend_) {
    bits = kBitProbed;
    why = "this build has no AMD FidelityFX frame generation";
  } else {
    const FgBackendStatus st = backend_->Status();
    const bool probed = st.state == FgBackendState::kAvailable || st.state == FgBackendState::kUnavailable;
    const bool failed = st.failed || failed_;
    if (probed) bits |= kBitProbed;
    if (st.state == FgBackendState::kAvailable && !failed) bits |= kBitAvailable;
    if (failed) bits |= kBitFailed;
    if (st.state == FgBackendState::kUnavailable || failed) {
      why = st.reason.empty() ? std::string("frame generation could not be switched on") : st.reason;
    }
  }
  if (active_) bits |= kBitRunning;
  if (memory_blocked_) {
    bits |= kBitNoRoom;
    if (why.empty()) {
      why = memory_reason_;
    }
  }
  (void)setting;
  const uint32_t before = g_bits.exchange(bits, std::memory_order_acq_rel);
  if (!why.empty() || before != bits) {
    std::lock_guard lock(g_why_mutex);
    g_why = why;
  }
}

FrameGeneration::Change FrameGeneration::Poll() {
  if (!initialized_) {
    return Change::kNone;
  }
  UpdateDisplayRefresh();
  const bool setting = WantedBySetting();
  FgBackendStatus st;
  if (backend_) {
    st = backend_->Status();
  }
  const bool can_run = backend_ && st.state == FgBackendState::kAvailable && !st.failed && !failed_;
  bool wanted = setting && can_run;
  if (!wanted) {
    ClearMemoryBlock();  // a setting that is off, or cannot run, has nothing to wait for
  } else {
    if (st.no_memory && !memory_blocked_) {
      BlockForMemory(st.reason);  // the runtime ran out of video memory making the context
    }
    if (active_) {
      wanted = !memory_blocked_;  // set by Generate, AcquireSlot or the line above: switched off
    } else if (!switching_on_) {
      wanted = RoomToSwitchOn();
    }
  }
  wanted_ = wanted;
  Publish(setting);
  if (setting && !can_run && !logged_unavailable_ &&
      (!backend_ || st.state == FgBackendState::kUnavailable || st.failed || failed_)) {
    logged_unavailable_ = true;
    REXLOG_INFO("[gpu] frame generation is chosen but cannot run here: {}",
                !backend_ ? "this build has no AMD FidelityFX frame generation"
                          : (st.reason.empty() ? "it failed earlier in this session" : st.reason));
  }
  // The groundwork follows the setting while it can run: the temporal system keeps the depth and the
  // vectors, the frame layers keep the hudless colour and the UI layer.
  const bool need = wanted_ || active_;
  temporal_->RequestVectors(need);
  layers_->Request(FrameLayers::kRequesterFrameGeneration, need);
  if (wanted_ && !active_) {
    return Change::kOn;
  }
  if (active_ && !wanted_) {
    return Change::kOff;
  }
  return Change::kNone;
}

bool FrameGeneration::MakeGenerationPasses() {
  if (prepare_pipeline_ && compose_pipeline_) {
    return true;
  }
  const HostShaderBlob prepare = FrameGenerationPreparePixelShader();
  const HostShaderBlob compose = FrameGenerationComposePixelShader();
  prepare_shader_ = passes_->CreateShader(prepare.data, prepare.size, prepare.name);
  compose_shader_ = passes_->CreateShader(compose.data, compose.size, compose.name);
  if (!prepare_shader_ || !compose_shader_) {
    return false;
  }
  prepare_pipeline_ =
      passes_->CreatePipeline(*prepare_shader_, kGenerationFormat, "frame generation prepare");
  compose_pipeline_ =
      passes_->CreatePipeline(*compose_shader_, kGenerationFormat, "frame generation compose");
  return prepare_pipeline_ && compose_pipeline_;
}

// ---------------------------------------------------------------------------
// video memory (frame_generation.h "Memory")
// ---------------------------------------------------------------------------

void FrameGeneration::GenerationSize(uint32_t src_w, uint32_t src_h, uint32_t window_w,
                                     uint32_t window_h, uint32_t* gen_w, uint32_t* gen_h) {
  *gen_w = src_w;
  *gen_h = src_h;
  if (window_w != 0 && window_h != 0 && src_w != 0 && src_h != 0) {
    const double scale = std::min(double(window_w) / double(src_w), double(window_h) / double(src_h));
    if (scale < 1.0) {
      *gen_w = std::max<uint32_t>(uint32_t(std::lround(double(src_w) * scale)), 1u);
      *gen_h = std::max<uint32_t>(uint32_t(std::lround(double(src_h) * scale)), 1u);
    }
  }
}

std::string FrameGeneration::MemoryProblem(const FgContextRequest& request, uint64_t other_bytes,
                                           std::string* figures) const {
  if (!device_ || !backend_) {
    return {};
  }
  // The three generation images, what the caller adds (the present images and the groundwork when
  // switching on) and AMD's context as the last context the runtime reported on suggests.
  const uint64_t images =
      3 * TextureBytes(request.generation_size[0], request.generation_size[1], 1, kGenerationFormat) +
      other_bytes;
  const uint64_t context = backend_->EstimateContextBytes(request);
  const uint64_t need = images + context;
  const backend::VideoMemory memory = device_->QueryVideoMemory();
  const std::string account = std::format(
      "it needs about {} MiB ({} MiB of images, about {} MiB for AMD's context for {}x{}) and {}",
      need >> 20, images >> 20, context >> 20, request.generation_size[0], request.generation_size[1],
      memory.budget != 0 ? std::format("{} of {} MiB are in use", memory.used >> 20, memory.budget >> 20)
                         : std::string("the card reports no budget"));
  if (figures) {
    *figures = account;
  }
  if (VideoMemoryBudget::HasRoomFor(memory, need)) {
    return {};
  }
  return "not enough free video memory: " + account;
}

std::string FrameGeneration::SwitchOnProblem(std::string* figures) const {
  if (!swap_chain_ || !device_ || !layers_ || !temporal_) {
    return {};
  }
  const uint32_t window_w = swap_chain_->width();
  const uint32_t window_h = swap_chain_->height();
  if (window_w == 0 || window_h == 0) {
    return {};
  }
  // The picture the last generation saw (forgotten when it went off), else one that fills the
  // window: the generated frame is never larger than the window, so this is the most it can need.
  const uint32_t src_w = last_source_[0] != 0 ? last_source_[0] : window_w;
  const uint32_t src_h = last_source_[1] != 0 ? last_source_[1] : window_h;
  FgContextRequest request;
  GenerationSize(src_w, src_h, window_w, window_h, &request.generation_size[0],
                 &request.generation_size[1]);
  request.render_size[0] = request.generation_size[0];
  request.render_size[1] = request.generation_size[1];
  // What switching on makes besides the generation images and the context: the present images (two
  // slots of a pair, at the window's size in the swap chain's format) and the groundwork it asks
  // the others for, unless they have it already: the frame layers' two 8-bit images at the
  // picture's size, and the temporal system's kept depth (two, R32_FLOAT) and vectors (RG16F), at
  // most the generated frame's size.
  uint64_t other = uint64_t(kSlots) * 2 *
                   TextureBytes(window_w, window_h, 1, swap_chain_->format());
  if (!layers_->active()) {
    other += 2 * TextureBytes(src_w, src_h, 1, plume::RenderFormat::R8G8B8A8_UNORM);
  }
  if (temporal_->motion().HeldBytes() == 0) {
    other += uint64_t(2 * 4 + 4) * request.generation_size[0] * request.generation_size[1];
  }
  return MemoryProblem(request, other, figures);
}

bool FrameGeneration::RoomToSwitchOn() {
  const Clock::time_point now = Clock::now();
  if (memory_blocked_ && now < memory_retry_) {
    return false;
  }
  std::string figures;
  const std::string problem = SwitchOnProblem(&figures);
  if (!problem.empty()) {
    BlockForMemory(problem);
    return false;
  }
  if (memory_blocked_ && backend_) {
    backend_->Release();  // forgets an answer that the runtime ran out of memory
  }
  REXLOG_INFO("[gpu] frame generation: {}: there is room for it{}", figures,
              memory_blocked_ ? " now, switching it on" : "");
  ClearMemoryBlock();
  return true;
}

void FrameGeneration::BlockForMemory(const std::string& problem) {
  const Clock::time_point now = Clock::now();
  if (memory_blocked_ && now < memory_retry_) {
    return;  // already waiting: a second sign in the same stretch does not move the wait
  }
  memory_blocked_ = true;
  memory_reason_ = problem;
  memory_retry_ = now + memory_delay_;
  if (!memory_logged_) {
    memory_logged_ = true;
    REXLOG_INFO("[gpu] frame generation: {}; it is {} and looked at again after {} s, then less "
                "often (up to {} s); nothing else is released to make room for it",
                problem, active_ ? "switched off" : "not switched on", memory_delay_.count(),
                kMemoryRetryMax.count());
  }
  memory_delay_ = std::min(memory_delay_ * 2, kMemoryRetryMax);
}

bool FrameGeneration::ImagesFailed(const char* what, uint64_t bytes) {
  const backend::VideoMemory memory = device_->QueryVideoMemory();
  if (memory.budget != 0 && VideoMemoryBudget::HasRoomFor(memory, bytes)) {
    return false;  // the card has room: not a matter of waiting for it
  }
  BlockForMemory(std::format("not enough free video memory: its {} images could not be made ({} of "
                             "{} MiB are in use)",
                             what, memory.used >> 20, memory.budget >> 20));
  return true;
}

void FrameGeneration::ClearMemoryBlock() {
  memory_blocked_ = false;
  memory_logged_ = false;
  memory_reason_.clear();
  memory_retry_ = Clock::time_point{};
  memory_delay_ = kMemoryRetryFirst;
}

bool FrameGeneration::MakeQueue(plume::RenderFormat format) {
  plume::RenderDevice* dev = device_->device();
  if (!present_queue_) {
    present_queue_ = dev->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (!present_queue_ || backend::IsNullBacked(present_queue_.get())) {
      present_queue_.reset();
      REXLOG_ERROR("[gpu] frame generation: a second queue for the presents could not be made");
      return false;
    }
    bool ok = true;
    for (Lane& lane : lanes_) {
      lane.acquire = dev->createCommandSemaphore();
      lane.list = present_queue_->createCommandList();
      lane.fence = dev->createCommandFence();
      ok = ok && lane.acquire && lane.list && lane.fence;
    }
    idle_list_ = present_queue_->createCommandList();
    idle_fence_ = dev->createCommandFence();
    ok = ok && idle_list_ && idle_fence_;
    for (Slot& slot : slots_) {
      if (!slot.marker) {
        slot.marker = device_->queue()->createCommandList();
        slot.frame_done = dev->createCommandFence();
      }
      ok = ok && slot.marker && slot.frame_done;
    }
    if (!ok) {
      ReleaseQueue();
      REXLOG_ERROR("[gpu] frame generation: the present queue's lists and fences could not be made");
      return false;
    }
  }
  if (!copy_shader_) {
    const HostShaderBlob copy = FrameGenerationCopyPixelShader();
    copy_shader_ = passes_->CreateShader(copy.data, copy.size, copy.name);
  }
  if (!copy_shader_) {
    return false;
  }
  if (!copy_pipeline_ || copy_format_ != format) {
    copy_pipeline_ = passes_->CreatePipeline(*copy_shader_, format, "frame generation copy");
    copy_format_ = format;
  }
  return copy_pipeline_ != nullptr;
}

bool FrameGeneration::PrepareSwitchOn(plume::RenderFormat format) {
  if (!MakeGenerationPasses() || !MakeQueue(format)) {
    failed_ = true;
    REXLOG_ERROR("[gpu] frame generation: it could not be set up (a pipeline or the present queue "
                 "could not be made, reason above); it stays off for this session");
    return false;
  }
  switching_on_ = true;
  return true;
}

void FrameGeneration::OnSwapChainChanged() {
  if (!present_queue_ || !swap_chain_) {
    return;
  }
  if (!MakeQueue(swap_chain_->format())) {
    failed_ = true;
    REXLOG_ERROR("[gpu] frame generation: the copy pass for the new swap chain could not be made");
  }
}

void FrameGeneration::CompleteSwitch(Change change, bool ok) {
  if (change == Change::kOn) {
    switching_on_ = false;
    if (!ok) {
      failed_ = true;
      ReleaseQueue();
      REXLOG_ERROR("[gpu] frame generation: the swap chain could not be made on the present queue; "
                   "it stays off for this session");
      Publish(true);
      return;
    }
    active_ = true;
    {
      std::lock_guard lock(mu_);
      stop_ = false;
      pause_ = false;
      idle_ = false;
      busy_ = false;
      queue_.clear();
      for (bool& free : slot_free_) free = true;
      running_ = true;
    }
    stalled_ = false;
    slot_cursor_ = 0;
    interval_count_ = 0;
    interval_next_ = 0;
    last_present_us_ = 0;
    last_handed_us_ = 0;
    last_generated_frame_ = ~uint64_t(0);
    idle_guest_frames_ = 0;
    thread_ = std::thread([this] { PacerMain(); });
    Publish(true);
    REXLOG_INFO("[gpu] frame generation: on (FSR 3.1; the swap chain is on the present queue, {}, "
                "the display runs at {} Hz, the frame limit counts rendered frames: {})",
                REXCVAR_GET(mcla_gpu_frame_generation_tearing) ? "presents are immediate (they may tear)"
                                                                : "presents are FIFO",
                refresh_hz_,
                REXCVAR_GET(mcla_gpu_frame_generation_fps_limit) < 0
                    ? std::format("HALF DISPLAY, {}", HalfDisplayCap(refresh_hz_) != 0
                                                          ? std::to_string(HalfDisplayCap(refresh_hz_))
                                                          : std::string("not offered here, fps_limit"))
                    : std::to_string(REXCVAR_GET(mcla_gpu_frame_generation_fps_limit)));
    return;
  }
  // Off: the swap chain is back on the render queue (or could not be made at all).
  StopPacer();
  active_ = false;
  switching_on_ = false;
  last_source_[0] = last_source_[1] = 0;  // the next switch on is estimated from the window
  ReleaseQueue();
  ReleaseResources("frame generation is off");
  for (Slot& slot : slots_) {
    passes_->ReleaseImage(slot.generated);
    passes_->ReleaseImage(slot.real);
  }
  Publish(false);
  REXLOG_INFO("[gpu] frame generation: off{}; the swap chain is back on the render queue",
              ok ? "" : " (the switch had trouble: see above)");
}

void FrameGeneration::ReleaseResources(const char* why) {
  if (backend_) {
    backend_->Release();
  }
  const bool had = color_.texture || generated_.texture || composed_.texture;
  passes_->ReleaseImage(color_);
  passes_->ReleaseImage(generated_);
  passes_->ReleaseImage(composed_);
  have_request_ = false;  // the next context is asked for with a look at the budget again
  last_generated_frame_ = ~uint64_t(0);
  idle_guest_frames_ = 0;
  if (had && !released_) {
    REXLOG_INFO("[gpu] frame generation: the context and the generation images are released ({})",
                why);
  }
  released_ = true;
}

void FrameGeneration::NoteNotGenerated() {
  if (++idle_guest_frames_ >= kReleaseAfterFrames && !released_) {
    ReleaseResources(std::format("nothing to generate from for {} guest frames", kReleaseAfterFrames)
                         .c_str());
  }
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool FrameGeneration::AcquireSlot(uint32_t width, uint32_t height, plume::RenderFormat format,
                                  Pair* out) {
  const uint32_t index = slot_cursor_;
  {
    std::unique_lock lock(mu_);
    if (!slot_free_[index]) {
      if (stalled_) {
        ++stats_.dropped;
        return false;
      }
      const auto begin = Clock::now();
      ++stats_.slot_waits;
      const ScopedGpuWait gpu_wait;  // the render thread waits for the pacer, which waits for the GPU
      cv_.wait_for(lock, kStallTimeout, [&] { return slot_free_[index] || stop_; });
      stats_.slot_wait_ms +=
          std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
      if (!slot_free_[index]) {
        stalled_ = true;
        ++stats_.dropped;
        REXLOG_WARN("[gpu] frame generation: the pacer took no pair for {} ms; it is called stalled "
                    "and frames are dropped until it takes one (a present or an acquire is stuck "
                    "in the driver, or the window is hidden)",
                    kStallTimeout.count());
        return false;
      }
    }
    slot_free_[index] = false;
  }
  Slot& slot = slots_[index];
  if (!passes_->EnsureImage(slot.generated, width, height, format, "frame generation present image") ||
      !passes_->EnsureImage(slot.real, width, height, format, "frame generation present image")) {
    {
      std::lock_guard lock(mu_);
      slot_free_[index] = true;
    }
    if (width != 0 && height != 0) {
      // Most likely the card is full (Device::CreateTexture said so): off until there is room again,
      // not frames dropped for as long as it lasts.
      ImagesFailed("present", 2 * TextureBytes(width, height, 1, format));
    }
    return false;
  }
  out->index = index;
  out->generated = &slot.generated;
  out->real = &slot.real;
  return true;
}

void FrameGeneration::ReturnSlot(const Pair& pair) {
  std::lock_guard lock(mu_);
  slot_free_[pair.index] = true;
  cv_.notify_all();
}

bool FrameGeneration::Submit(const Pair& pair, bool has_generated, uint64_t latency_frame,
                             bool guest_frame) {
  Slot& slot = slots_[pair.index];
  // Behind the frame's list, on the render queue: its fence is complete when the frame's work is.
  slot.marker->begin();
  slot.marker->end();
  const plume::RenderCommandList* lists[] = {slot.marker.get()};
  device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, slot.frame_done.get());
  {
    std::lock_guard lock(mu_);
    Item item;
    item.slot = pair.index;
    item.has_generated = has_generated;
    item.latency_frame = latency_frame;
    item.guest_frame = guest_frame;
    item.handed_us = NowUs();
    queue_.push_back(item);
    cv_.notify_all();
  }
  slot_cursor_ = (slot_cursor_ + 1) % kSlots;
  return true;
}

FrameGeneration::Generated FrameGeneration::Generate(plume::RenderCommandList* list,
                                                     const GenerateArgs& args) {
  Generated none;
  if (!args.guest_frame || !backend_) {
    return none;  // an idle present repeats the last frame: nothing to generate, nothing counted
  }
  ++stats_.guest_frames;
  const auto skip = [&](uint64_t& counter) {
    ++counter;
    NoteNotGenerated();
    return none;
  };
  // No room for it: nothing is made, and the next frame boundary switches it off (Poll).
  if (memory_blocked_) {
    return skip(stats_.not_generated_memory);
  }
  // The HDR debug views and the test pattern show something else than the frame.
  if (REXCVAR_GET(mcla_gpu_hdr_layers_view) != 0 || REXCVAR_GET(mcla_gpu_hdr_test_pattern)) {
    return skip(stats_.not_generated_frame);
  }
  const TemporalSystem::FrameState& fr = temporal_->frame();
  const TemporalMotion::Kept kept = temporal_->motion().kept();
  if (!fr.anchor_ok || !kept.valid || kept.frame != fr.frame || !kept.depth || !kept.motion ||
      !kept.depth->texture || !kept.motion->texture) {
    return skip(stats_.not_generated_frame);
  }
  const FrameLayers::Frame layer = layers_->presentable();
  const uint32_t src_w = args.front_width;
  const uint32_t src_h = args.front_height;
  if (!layer.valid || !layer.IsPictureOf(args.front) || src_w == 0 || src_h == 0 ||
      layer.width < src_w || layer.height < src_h || !layer.hudless || !layer.ui) {
    return skip(stats_.not_generated_frame);
  }
  const HostImage* float_scene = layers_->FloatSceneOf(layer, src_w, src_h);
  const HostImage* scene = float_scene ? float_scene : layer.hudless;
  if (!scene->texture || scene->slot == kInvalidBindlessSlot || !layer.ui->texture ||
      layer.ui->slot == kInvalidBindlessSlot) {
    return skip(stats_.not_generated_frame);
  }

  // The size the generation works at: the picture's, or where the window shows it smaller exactly the
  // size it is shown at (the fork's finding: 1.7 to 2.5 ms at 4K against 0.85 to 1.6 at 1440p).
  uint32_t gen_w = src_w;
  uint32_t gen_h = src_h;
  GenerationSize(src_w, src_h, args.window_width, args.window_height, &gen_w, &gen_h);
  last_source_[0] = src_w;
  last_source_[1] = src_h;
  FgContextRequest request;
  request.render_size[0] = kept.depth->width;
  request.render_size[1] = kept.depth->height;
  request.generation_size[0] = gen_w;
  request.generation_size[1] = gen_h;
  if (!have_request_ || !(requested_ == request)) {
    // A context of this size is new (the first, another size, or after the release): asked for
    // only where it and the images that go with it fit under the budget's line (the old context
    // of another size is still there while the new one is made, so nothing is taken off the need).
    std::string figures;
    const std::string problem = MemoryProblem(request, 0, &figures);
    if (!problem.empty()) {
      BlockForMemory(problem);
      return skip(stats_.not_generated_memory);
    }
    REXLOG_INFO("[gpu] frame generation: {}: asking for a context for the generated frame at "
                "{}x{} (render {}x{})",
                figures, gen_w, gen_h, request.render_size[0], request.render_size[1]);
    requested_ = request;
    have_request_ = true;
  }
  backend_->Request(request);
  if (!backend_->Ready(request)) {
    last_generated_frame_ = ~uint64_t(0);  // the first generation of a context starts afresh
    return skip(stats_.not_generated_context);
  }
  if (!passes_->EnsureImage(color_, gen_w, gen_h, kGenerationFormat, "frame generation scene") ||
      !passes_->EnsureImage(generated_, gen_w, gen_h, kGenerationFormat, "frame generation output",
                            kOutputFlags) ||
      !passes_->EnsureImage(composed_, gen_w, gen_h, kGenerationFormat, "frame generation composed")) {
    if (ImagesFailed("generation", 3 * TextureBytes(gen_w, gen_h, 1, kGenerationFormat))) {
      return skip(stats_.not_generated_memory);
    }
    return skip(stats_.not_generated_frame);
  }
  released_ = false;

  const bool reset = fr.reset != 0 || last_generated_frame_ == ~uint64_t(0) ||
                     last_generated_frame_ + 1 != fr.frame;

  // 1. The scene at the generation size, in the codes the interpolation reads.
  pass_timing_->Mark(list, "frame generation");
  {
    PreparePush push{};
    push.scene = scene->slot;
    push.flags = float_scene ? kFlagFloatScene : 0u;
    push.src_width = src_w;
    push.src_height = src_h;
    push.out_width = gen_w;
    push.out_height = gen_h;
    passes_->DrawInto(list, color_, prepare_pipeline_.get(), &push, sizeof(push));
  }

  // 2. The interpolation. Outside a render pass: the inputs go to the compute stage's read state
  // (D3D12 NON_PIXEL_SHADER_RESOURCE, Vulkan SHADER_READ_ONLY_OPTIMAL), the output to general; the
  // runtime takes them back to those states; they then go back to what the passes read them in.
  namespace Stage = plume::RenderBarrierStage;
  FgInputs in;
  in.frame_id = ++frame_id_;
  in.reset = reset;
  in.render_size[0] = kept.depth->width;
  in.render_size[1] = kept.depth->height;
  in.generation_size[0] = gen_w;
  in.generation_size[1] = gen_h;
  in.color = {color_.texture.get(), gen_w, gen_h, kGenerationFormat, color_.flags};
  in.depth = {kept.depth->texture.get(), kept.depth->width, kept.depth->height, kept.depth->format,
              kept.depth->flags};
  in.motion = {kept.motion->texture.get(), kept.motion->width, kept.motion->height,
               kept.motion->format, kept.motion->flags};
  in.output = {generated_.texture.get(), gen_w, gen_h, kGenerationFormat, kOutputFlags};
  in.jitter[0] = fr.jitter[0];
  in.jitter[1] = fr.jitter[1];
  in.frame_time_ms = fr.frame_time_ms;
  in.camera_near = float(fr.camera.near_plane);
  in.camera_far = float(fr.camera.far_plane);
  in.camera_fov_y = float(fr.camera.fov_y);
  for (int i = 0; i < 3; ++i) {
    in.camera_position[i] = float(fr.camera.position[i]);
    in.camera_up[i] = float(fr.camera.up[i]);
    in.camera_right[i] = float(fr.camera.right[i]);
    in.camera_forward[i] = float(fr.camera.forward[i]);
  }
  plume::RenderTextureBarrier to_read[3] = {
      plume::RenderTextureBarrier(color_.texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::COMPUTE, to_read, 3);
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(generated_.texture.get(),
                                                              plume::RenderTextureLayout::GENERAL));
  const bool ok = backend_->Generate(list, in);
  backend::InvalidateAfterExternalWork(list);
  list->barriers(Stage::GRAPHICS_AND_COMPUTE,
                 plume::RenderTextureBarrier(color_.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  plume::RenderTextureBarrier back[2] = {
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::GRAPHICS, back, 2);
  list->barriers(Stage::GRAPHICS_AND_COMPUTE,
                 plume::RenderTextureBarrier(generated_.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  generated_.layout = plume::RenderTextureLayout::SHADER_READ;
  if (!ok) {
    // The backend logged it and is off for the session: Poll switches frame generation off.
    last_generated_frame_ = ~uint64_t(0);
    return skip(stats_.not_generated_frame);
  }
  last_generated_frame_ = fr.frame;
  if (reset) {
    // The history starts again with this frame (it was fed to the interpolation): a generated frame
    // between two unrelated pictures is not shown.
    return skip(stats_.not_generated_reset);
  }

  // 3. The generated scene with this frame's UI over it, as the real frame composes it.
  {
    ComposePush push{};
    push.generated = generated_.slot;
    push.ui = layer.ui->slot;
    push.flags = float_scene ? kFlagFloatScene : 0u;
    push.src_width = src_w;
    push.src_height = src_h;
    push.out_width = gen_w;
    push.out_height = gen_h;
    passes_->DrawInto(list, composed_, compose_pipeline_.get(), &push, sizeof(push));
  }
  ++stats_.generated;
  idle_guest_frames_ = 0;
  Generated result;
  result.image = &composed_;
  result.float_scene = float_scene != nullptr;
  return result;
}

uint64_t FrameGeneration::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  add(color_);
  add(generated_);
  add(composed_);
  for (const Slot& slot : slots_) {
    add(slot.generated);
    add(slot.real);
  }
  if (backend_) {
    bytes += backend_->Status().context_bytes;
  }
  return bytes;
}

std::string FrameGeneration::TakeSummary() {
  PacerStats pacer;
  {
    std::lock_guard lock(mu_);
    pacer = pacer_stats_;
    pacer_stats_ = PacerStats{};
  }
  const Stats s = stats_;
  stats_ = Stats{};
  const double mean_gap = pacer.gaps != 0 ? pacer.gap_sum_ms / double(pacer.gaps) : 0.0;
  return std::format(
      "frame generation: {} guest frames, {} generated; not generated: {} no scene or layers, {} "
      "context not ready, {} no room in video memory, {} history starting again; {} dropped (no "
      "free slot), {} slot waits ({:.1f} ms) | pacer: {} presents ({} generated), mean gap {:.2f} "
      "ms, longest {:.2f} ms, {} more than 1 ms late, {} failed | {:.0f} MiB",
      s.guest_frames, s.generated, s.not_generated_frame, s.not_generated_context,
      s.not_generated_memory, s.not_generated_reset, s.dropped, s.slot_waits, s.slot_wait_ms,
      pacer.presents,
      pacer.generated_presents, mean_gap, pacer.gap_max_ms, pacer.late, pacer.failed,
      double(HeldBytes()) / double(1u << 20));
}

// ---------------------------------------------------------------------------
// the pacer
// ---------------------------------------------------------------------------

void FrameGeneration::PausePacer() {
  {
    std::unique_lock lock(mu_);
    if (!running_) {
      return;
    }
    pause_ = true;
    cv_.notify_all();
    const bool parked =
        cv_.wait_for(lock, std::chrono::seconds(5), [this] { return !running_ || (idle_ && queue_.empty()); });
    if (!parked) {
      REXLOG_WARN("[gpu] frame generation: the pacer did not stop presenting in 5 s; the swap chain "
                  "is changed under it");
    }
  }
  DrainPresentQueue();
}

void FrameGeneration::ResumePacer() {
  std::lock_guard lock(mu_);
  pause_ = false;
  cv_.notify_all();
}

void FrameGeneration::DrainPresentQueue() {
  // An empty list and a fence of their own behind everything the present queue was given, presents
  // included (as FrameRing::WaitIdle does for the render queue).
  if (!present_queue_ || !idle_list_ || !idle_fence_) {
    return;
  }
  idle_list_->begin();
  idle_list_->end();
  const plume::RenderCommandList* lists[] = {idle_list_.get()};
  present_queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, idle_fence_.get());
  present_queue_->waitForCommandFence(idle_fence_.get());
}

void FrameGeneration::WaitUntil(int64_t target_us) {
  // A high-resolution waitable timer wakes a fraction of a millisecond after its time, where a sleep
  // on the 1 ms system timer can be 1.5 ms late (the frame limiter does the same,
  // hooks/frame_timing.cpp); it sleeps to 0.5 ms before the target and spins the rest.
  constexpr int64_t kSpinUs = 500;
  int64_t remaining = target_us - NowUs();
  if (remaining > kSpinUs + 100) {
    if (timer_) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>((remaining - kSpinUs) * 10);  // 100 ns units, relative
      if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(static_cast<HANDLE>(timer_), 100);
      }
    } else if (remaining > 3000) {
      // No high-resolution timer (before Windows 10 1803): the system timer's granularity.
      std::this_thread::sleep_for(std::chrono::microseconds(remaining - 3000));
    }
  }
  while (NowUs() < target_us) {
    YieldProcessor();
  }
}

void FrameGeneration::PacerMain() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
  for (;;) {
    Item item;
    {
      std::unique_lock lock(mu_);
      for (;;) {
        if (!queue_.empty()) {
          item = queue_.front();
          queue_.pop_front();
          busy_ = true;
          break;
        }
        if (stop_) {
          running_ = false;
          cv_.notify_all();
          lock.unlock();
          if (timer_) {
            CloseHandle(static_cast<HANDLE>(timer_));
            timer_ = nullptr;
          }
          return;
        }
        if (pause_) {
          idle_ = true;
          cv_.notify_all();
          cv_.wait(lock, [this] { return !pause_ || stop_ || !queue_.empty(); });
          idle_ = false;
          continue;
        }
        cv_.wait(lock, [this] { return stop_ || pause_ || !queue_.empty(); });
      }
    }
    Process(item);
    {
      std::lock_guard lock(mu_);
      busy_ = false;
      slot_free_[item.slot] = true;
      stalled_ = false;
      cv_.notify_all();
    }
  }
}

void FrameGeneration::Process(const Item& item) {
  Slot& slot = slots_[item.slot];
  // The frame's work (and so both present images) is complete when its fence is.
  present_queue_->waitForCommandFence(slot.frame_done.get());

  // The rendered interval as the pacer sees it: the time between the hand-overs of the last ten guest
  // frames; a pause of over half a second starts the window again, a shorter hitch stays in it.
  if (item.guest_frame) {
    if (last_handed_us_ != 0) {
      const int64_t gap = item.handed_us - last_handed_us_;
      if (gap > 0 && gap < 500'000) {
        intervals_[interval_next_] = gap;
        interval_next_ = (interval_next_ + 1) % kWindow;
        interval_count_ = std::min(interval_count_ + 1, kWindow);
      } else {
        interval_count_ = 0;
        interval_next_ = 0;
      }
    }
    last_handed_us_ = item.handed_us;
  }
  // AMD's formula: the presents are half the rendered interval apart, less a tenth of its mean
  // deviation and 0.1 ms.
  int64_t delay_us = 0;
  if (interval_count_ >= 2) {
    double mean = 0.0;
    for (uint32_t i = 0; i < interval_count_; ++i) {
      mean += double(intervals_[i]);
    }
    mean /= double(interval_count_);
    double deviation = 0.0;
    for (uint32_t i = 0; i < interval_count_; ++i) {
      deviation += std::fabs(double(intervals_[i]) - mean);
    }
    deviation /= double(interval_count_);
    delay_us = std::max<int64_t>(int64_t(mean / 2.0 - 0.1 * deviation - 100.0), 0);
  }

  bool ok = true;
  const int64_t now = NowUs();
  const bool recent = last_present_us_ != 0 && now - last_present_us_ < 500'000;
  int64_t shown_at = 0;
  if (item.has_generated) {
    // The generated image goes as soon as it is ready but not before the last present plus the
    // delay; the real one that after it.
    shown_at = PresentImage(slot.generated, 0, recent ? last_present_us_ + delay_us : now, 0, &ok);
    std::lock_guard lock(mu_);
    ++pacer_stats_.generated_presents;
  }
  const int64_t real_due = (item.has_generated && shown_at != 0) ? shown_at + delay_us : now;
  PresentImage(slot.real, 1, real_due, item.latency_frame, &ok);
}

int64_t FrameGeneration::PresentImage(HostImage& image, uint32_t lane_index, int64_t due_us,
                                      uint64_t latency_frame, bool* ok) {
  *ok = false;
  const auto fail = [&]() {
    if (resize_flag_) {
      resize_flag_->store(true, std::memory_order_release);
    }
    std::lock_guard lock(mu_);
    ++pacer_stats_.failed;
    return int64_t(0);
  };
  if (!swap_chain_ || swap_chain_->empty() || image.slot == kInvalidBindlessSlot) {
    return fail();
  }
  if (swap_chain_->width() != image.width || swap_chain_->height() != image.height) {
    // The window changed size between the painting and now: the render thread makes the chain again
    // (before it does, the image is dropped: nothing was acquired).
    return fail();
  }
  Lane& lane = lanes_[lane_index];
  uint32_t back = 0;
  if (!swap_chain_->Acquire(lane.acquire.get(), &back)) {
    return fail();  // out of date: the render thread rebuilds the chain
  }
  // The copy: the present image texel for texel into the back buffer, a colour attachment write.
  plume::RenderCommandList* list = lane.list.get();
  plume::RenderTexture* target = swap_chain_->texture(back);
  list->begin();
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(swap_chain_->framebuffer(back));
  const plume::RenderViewport viewport{0.0f, 0.0f, float(swap_chain_->width()),
                                       float(swap_chain_->height())};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(swap_chain_->width()), int32_t(swap_chain_->height()));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(copy_pipeline_.get());
  CopyPush push{image.slot};
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  list->end();
  plume::RenderCommandSemaphore* waits[] = {lane.acquire.get()};
  plume::RenderCommandSemaphore* signals[] = {swap_chain_->render_semaphore(back)};
  const plume::RenderCommandList* lists[] = {list};
  present_queue_->executeCommandLists(lists, 1, waits, 1, signals, 1, lane.fence.get());
  present_queue_->waitForCommandFence(lane.fence.get());

  WaitUntil(due_us);
  if (latency_frame != 0) {
    latency::OnPresentBegin(latency_frame);
  }
  const bool presented = swap_chain_->Present(back);
  if (latency_frame != 0) {
    latency::OnPresentEnd(latency_frame);
  }
  const int64_t at = NowUs();
  {
    std::lock_guard lock(mu_);
    ++pacer_stats_.presents;
    if (at - due_us > 1000) {
      ++pacer_stats_.late;
    }
    if (last_present_us_ != 0) {
      const double gap = double(at - last_present_us_) / 1000.0;
      pacer_stats_.gap_sum_ms += gap;
      pacer_stats_.gap_max_ms = std::max(pacer_stats_.gap_max_ms, gap);
      ++pacer_stats_.gaps;
    }
  }
  last_present_us_ = at;
  if (!presented) {
    fail();  // the chain no longer matches the window: the render thread makes it again
    return at;
  }
  *ok = true;
  return at;
}

}  // namespace mcla::gpu
