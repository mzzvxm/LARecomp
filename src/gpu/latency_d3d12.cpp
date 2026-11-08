// MCLA GPU renderer -- the D3D12 half of the latency reducer: NVIDIA Reflex
// through NVAPI and AMD Anti-Lag 2 through the AMD driver's extension. See
// latency.h and latency_backend.h.
//
// NVAPI is not linked and has no SDK to download: nvapi64.dll comes with the
// NVIDIA driver, and the functions are found with nvapi_QueryInterface by the
// ids in NVAPI's public nvapi_interface.h (github.com/NVIDIA/nvapi, MIT,
// Copyright (c) NVIDIA CORPORATION & AFFILIATES). The structures and the marker
// enum below are the ones of its nvapi.h that Reflex uses, with the same layouts
// (packed as NVAPI packs them, 8; their sizes are checked). "NVIDIA Reflex" is
// used as plain text to say which feature this is.
//
// AMD's side is the Anti-Lag 2 SDK's Direct3D 12 header (thirdparty/amd-antilag2,
// MIT), which loads amdxc64.dll from the AMD driver.
//
// Design followed (not copied): the MCLA fork's d3d12_low_latency.cpp, written by
// the same author.

#include "latency_backend.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <rex/logging.h>

#include <plume_d3d12.h>

#include "amd-antilag2/ffx_antilag2_dx12.h"

namespace mcla::gpu::latency {

namespace {

// --- NVAPI ----------------------------------------------------------------------------

#pragma pack(push, 8)
using NvU8 = uint8_t;
using NvU32 = uint32_t;
using NvU64 = uint64_t;
using NvBool = NvU8;
using NvAPI_Status = int32_t;
constexpr NvAPI_Status kNvApiOk = 0;

// NV_LATENCY_MARKER_TYPE: the values this renderer sends.
enum NvLatencyMarkerType : uint32_t {
  SIMULATION_START = 0,
  SIMULATION_END = 1,
  RENDERSUBMIT_START = 2,
  RENDERSUBMIT_END = 3,
  PRESENT_START = 4,
  PRESENT_END = 5,
  INPUT_SAMPLE = 6,
};

struct NvSetSleepModeParams {
  NvU32 version;
  NvBool bLowLatencyMode;
  NvBool bLowLatencyBoost;
  NvU32 minimumIntervalUs;
  NvBool bUseMarkersToOptimize;
  NvBool bUseMinQueueTime;
  NvU8 rsvd[30];
};

struct NvLatencyMarkerParams {
  NvU32 version;
  NvU64 frameID;
  NvLatencyMarkerType markerType;
  NvU64 rsvd0;
  NvU8 rsvd[56];
};

// NV_GET_SLEEP_STATUS_PARAMS_V1 as the MCLA fork reads it (written by the same author): the driver's
// account of the sleep, with the frame generation fields after the interval.
struct NvGetSleepStatusParams {
  NvU32 version;
  NvBool bLowLatencyMode;
  NvBool bFsVrr;
  NvBool bCplVsyncOn;
  NvU32 sleepIntervalUs;
  NvBool bUseGameSleep;
  NvBool bFullscreenIFlip;
  NvU8 fgMultiplier;
  NvBool bDfgControl;
  NvU32 dfgFrameTimeTargetUs;
  NvU8 rsvd[114];
};
#pragma pack(pop)

static_assert(sizeof(NvSetSleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1");
static_assert(sizeof(NvLatencyMarkerParams) == 88, "NV_LATENCY_MARKER_PARAMS_V1");
static_assert(sizeof(NvGetSleepStatusParams) == 136, "NV_GET_SLEEP_STATUS_PARAMS_V1");

// MAKE_NVAPI_VERSION(type, 1).
template <typename T>
constexpr NvU32 NvVersion1() {
  return NvU32(sizeof(T) | (1u << 16));
}

constexpr uint32_t kIdInitialize = 0x0150e828;
constexpr uint32_t kIdSetSleepMode = 0xac1ca9e0;
constexpr uint32_t kIdSleep = 0x852cd1d2;
constexpr uint32_t kIdSetLatencyMarker = 0xd9984c05;
constexpr uint32_t kIdGetSleepStatus = 0xaef96ca1;

// Reflex's NvAPI_D3D_* take the device as an IUnknown.
using PfnQueryInterface = void*(__cdecl*)(uint32_t id);
using PfnInitialize = NvAPI_Status(__cdecl*)();
using PfnSetSleepMode = NvAPI_Status(__cdecl*)(IUnknown*, NvSetSleepModeParams*);
using PfnSleep = NvAPI_Status(__cdecl*)(IUnknown*);
using PfnSetLatencyMarker = NvAPI_Status(__cdecl*)(IUnknown*, NvLatencyMarkerParams*);
using PfnGetSleepStatus = NvAPI_Status(__cdecl*)(IUnknown*, NvGetSleepStatusParams*);

class NvapiReflex final : public Reducer {
 public:
  static std::shared_ptr<NvapiReflex> Create(ID3D12Device* device, std::string& why) {
    // The driver's own NVAPI, from the system folder only (not one beside the executable).
    HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      why = "nvapi64.dll is not installed (it comes with the NVIDIA driver)";
      return nullptr;
    }
    auto query =
        reinterpret_cast<PfnQueryInterface>(GetProcAddress(module, "nvapi_QueryInterface"));
    if (!query) {
      why = "nvapi64.dll has no nvapi_QueryInterface";
      return nullptr;
    }
    auto initialize = reinterpret_cast<PfnInitialize>(query(kIdInitialize));
    if (!initialize || initialize() != kNvApiOk) {
      why = "NvAPI_Initialize failed";
      return nullptr;
    }
    std::shared_ptr<NvapiReflex> reflex(new NvapiReflex(device));
    reflex->set_sleep_mode_ = reinterpret_cast<PfnSetSleepMode>(query(kIdSetSleepMode));
    reflex->sleep_ = reinterpret_cast<PfnSleep>(query(kIdSleep));
    reflex->set_latency_marker_ =
        reinterpret_cast<PfnSetLatencyMarker>(query(kIdSetLatencyMarker));
    // Only for the log and the switch off from DLSS frame generation: a driver without it still has Reflex.
    reflex->get_sleep_status_ = reinterpret_cast<PfnGetSleepStatus>(query(kIdGetSleepStatus));
    if (!reflex->set_sleep_mode_ || !reflex->sleep_ || !reflex->set_latency_marker_) {
      why = "the driver's NVAPI lacks the Reflex calls (a newer driver has them)";
      return nullptr;
    }
    // Whether the driver takes Reflex for this GPU is found out by asking it for the neutral mode,
    // which is the state a game that never calls NVAPI leaves it in (Reflex off, no limit): the
    // answer comes without turning anything on, and a player who leaves the setting off keeps
    // their driver's own. NVIDIA's samples test support the same way, by this call's result.
    // CHECK: not run here. A refusal at this point (a driver that wants a swap chain first) would
    // hide the row on a GPU that has Reflex; the log line says the status.
    const NvAPI_Status status = reflex->SetSleepMode(false, false);
    if (status != kNvApiOk) {
      why = "NvAPI_D3D_SetSleepMode refused the neutral mode (status " + std::to_string(status) +
            "): this GPU or driver has no Reflex";
      return nullptr;
    }
    return reflex;
  }

  const char* Name() const override { return "NVIDIA Reflex"; }
  bool IsReflex() const override { return true; }
  bool SupportsBoost() const override { return true; }
  // The fork measured it on an RTX 5080: 13 to 15 ms less from the swap to the screen at 3x
  // resolution, for 3 to 5% of the frame rate on D3D12 and nothing at native resolution.
  bool OnByDefault() const override { return true; }

  bool SetMode(bool on, bool boost) override {
    const NvAPI_Status status = SetSleepMode(on, boost);
    if (status != kNvApiOk) {
      REXLOG_WARN("[gpu] low latency: NvAPI_D3D_SetSleepMode failed ({})", status);
      return false;
    }
    // What the driver made of it (a mode is set a few times a session, so one line each): its
    // interval is where the driver's own frame cap shows (the NVIDIA app's Max Frame Rate).
    DriverPacing pacing;
    if (ReadPacing(&pacing)) {
      REXLOG_INFO("[gpu] low latency: the driver's pacing after Reflex was set {}: {}",
                  on ? (boost ? "on with boost" : "on") : "off", DescribePacing(pacing));
    }
    return true;
  }

  bool ReadPacing(DriverPacing* out) const override {
    if (!get_sleep_status_ || !out) {
      return false;
    }
    NvGetSleepStatusParams status = {};
    status.version = NvVersion1<NvGetSleepStatusParams>();
    if (get_sleep_status_(device_.Get(), &status) != kNvApiOk) {
      return false;
    }
    out->low_latency = status.bLowLatencyMode != 0;
    out->game_sleep = status.bUseGameSleep != 0;
    out->fullscreen_vrr = status.bFsVrr != 0;
    out->control_panel_vsync = status.bCplVsyncOn != 0;
    out->fullscreen_iflip = status.bFullscreenIFlip != 0;
    out->sleep_interval_us = status.sleepIntervalUs;
    out->fg_multiplier = status.fgMultiplier;
    out->dynamic_control = status.bDfgControl != 0;
    out->dynamic_target_us = status.dfgFrameTimeTargetUs;
    return true;
  }

  bool SupportsRearm() const override { return true; }

  // NvAPI_D3D_Sleep waits for the moment the driver picks and
  // cannot be cancelled, so unlike Vulkan's wait it has no bound: latency.cpp times it when it
  // returns and gives the reducer up for a while when it takes too long. A call that never
  // returns is not covered.
  bool Sleep(uint64_t) override { return sleep_(device_.Get()) == kNvApiOk; }

  void SetMarker(Marker marker, uint64_t frame_id) override {
    NvLatencyMarkerType type;
    switch (marker) {
      case Marker::kSimulationStart:
        type = SIMULATION_START;
        break;
      case Marker::kSimulationEnd:
        type = SIMULATION_END;
        break;
      case Marker::kRenderSubmitStart:
        type = RENDERSUBMIT_START;
        break;
      case Marker::kRenderSubmitEnd:
        type = RENDERSUBMIT_END;
        break;
      case Marker::kPresentStart:
        type = PRESENT_START;
        break;
      case Marker::kPresentEnd:
        type = PRESENT_END;
        break;
      case Marker::kInputSample:
        type = INPUT_SAMPLE;
        break;
      default:
        return;
    }
    NvLatencyMarkerParams params = {};
    params.version = NvVersion1<NvLatencyMarkerParams>();
    params.frameID = frame_id;
    params.markerType = type;
    set_latency_marker_(device_.Get(), &params);
  }

 private:
  explicit NvapiReflex(ID3D12Device* device) : device_(device) {}

  NvAPI_Status SetSleepMode(bool on, bool boost) {
    NvSetSleepModeParams params = {};
    params.version = NvVersion1<NvSetSleepModeParams>();
    params.bLowLatencyMode = on ? 1 : 0;
    params.bLowLatencyBoost = (on && boost) ? 1 : 0;
    // The markers decide where the wait goes (the neutral mode has no wait to place).
    params.bUseMarkersToOptimize = on ? 1 : 0;
    return set_sleep_mode_(device_.Get(), &params);
  }

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  PfnSetSleepMode set_sleep_mode_ = nullptr;
  PfnSleep sleep_ = nullptr;
  PfnSetLatencyMarker set_latency_marker_ = nullptr;
  PfnGetSleepStatus get_sleep_status_ = nullptr;
};

// --- AMD Anti-Lag 2 ---------------------------------------------------------------------

// AMD's SDK is built for its calls to run on different threads without a lock of ours: Update
// (the update thread, which sleeps in it) and MarkEndOfFrameRendering (the render thread). The
// SDK's Initialize turns the driver's Anti-Lag off, which is not for a player who never chose it
// here (their Adrenalin setting stays), so it is only called when the mode is first set on; Create
// only checks that the driver has the interface.
class AmdAntiLag2 final : public Reducer {
 public:
  static std::shared_ptr<AmdAntiLag2> Create(ID3D12Device* device, std::string& why) {
    // The driver's extension library, loaded by the Direct3D 12 driver of an AMD GPU.
    HMODULE module = GetModuleHandleA("amdxc64.dll");
    using PfnCreateInterface = HRESULT(__cdecl*)(IUnknown* outer, REFIID riid, void** object);
    auto create = module ? reinterpret_cast<PfnCreateInterface>(
                               GetProcAddress(module, "AmdExtD3DCreateInterface"))
                         : nullptr;
    if (!create) {
      why = "amdxc64.dll is not loaded (the AMD driver's extension library)";
      return nullptr;
    }
    AMD::AntiLag2DX12::IAmdExtAntiLagApi* api = nullptr;
    const HRESULT result = create(device, __uuidof(AMD::AntiLag2DX12::IAmdExtAntiLagApi),
                                  reinterpret_cast<void**>(&api));
    if (result != S_OK || !api) {
      why = "the AMD driver has no Anti-Lag 2 interface (0x" + HexResult(result) + ")";
      return nullptr;
    }
    api->Release();
    return std::shared_ptr<AmdAntiLag2>(new AmdAntiLag2(device));
  }

  ~AmdAntiLag2() override { Shutdown(); }

  const char* Name() const override { return "AMD Anti-Lag 2"; }
  bool IsReflex() const override { return false; }
  bool SetMode(bool on, bool) override {
    if (!on) {
      if (initialized_.load(std::memory_order_acquire)) {
        // Told off once (a disabled Update has no wait).
        enabled_ = false;
        AMD::AntiLag2DX12::Update(&context_, false, 0);
      }
      return true;
    }
    if (!EnsureInitialized()) {
      return false;
    }
    // The state goes to the driver with the frame's Update (Sleep, just after this).
    enabled_ = true;
    return true;
  }

  // The per-frame call goes just before the input is polled: it waits where the driver wants.
  bool Sleep(uint64_t) override {
    return initialized_.load(std::memory_order_acquire) &&
           SUCCEEDED(AMD::AntiLag2DX12::Update(&context_, enabled_, 0));
  }

  void SetMarker(Marker marker, uint64_t) override {
    // Once the frame's main rendering has been submitted, before the present: the submission is
    // made before the present is called, so this is where it is sent from.
    if (marker == Marker::kPresentStart && initialized_.load(std::memory_order_acquire)) {
      AMD::AntiLag2DX12::MarkEndOfFrameRendering(&context_);
    }
  }

  void Shutdown() override {
    if (initialized_.exchange(false, std::memory_order_acq_rel)) {
      AMD::AntiLag2DX12::DeInitialize(&context_);
    }
  }

 private:
  explicit AmdAntiLag2(ID3D12Device* device) : device_(device) {}

  static std::string HexResult(HRESULT result) {
    char text[16];
    std::snprintf(text, sizeof text, "%08X", uint32_t(result));
    return text;
  }

  bool EnsureInitialized() {
    if (initialized_.load(std::memory_order_acquire)) {
      return true;
    }
    if (initialize_failed_) {
      return false;
    }
    const HRESULT result = AMD::AntiLag2DX12::Initialize(&context_, device_.Get());
    if (result != S_OK) {
      REXLOG_WARN("[gpu] low latency: Anti-Lag 2 could not start on this device (0x{})",
                  HexResult(result));
      initialize_failed_ = true;
      return false;
    }
    initialized_.store(true, std::memory_order_release);
    return true;
  }

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  AMD::AntiLag2DX12::Context context_;
  // Set by the update thread, read by the render thread.
  std::atomic<bool> initialized_{false};
  // The update thread's.
  bool initialize_failed_ = false;
  bool enabled_ = false;
};

}  // namespace

std::shared_ptr<Reducer> CreateD3D12Reducer(plume::RenderDevice* device, std::string& why) {
  auto* d3d12 = static_cast<plume::D3D12Device*>(device);
  if (!d3d12 || !d3d12->d3d) {
    why = "no D3D12 device";
    return nullptr;
  }
  // The rendering device's vendor decides, not the display's.
  switch (device->getDescription().vendor) {
    case plume::RenderDeviceVendor::NVIDIA:
      return NvapiReflex::Create(d3d12->d3d, why);
    case plume::RenderDeviceVendor::AMD:
      return AmdAntiLag2::Create(d3d12->d3d, why);
    default:
      why = "only NVIDIA GPUs (Reflex) and AMD GPUs (Anti-Lag 2) have a reducer the game can use";
      return nullptr;
  }
}

}  // namespace mcla::gpu::latency
