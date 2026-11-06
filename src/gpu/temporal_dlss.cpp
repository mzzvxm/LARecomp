// MCLA GPU renderer -- the DLSS (Streamline) upscaler backend. See temporal_dlss.h.

#include "temporal_dlss.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// NVIDIA Streamline's headers (MIT, fetched by cmake/FetchStreamline.cmake, never committed). Only
// their types and constants are used: every function comes from sl.interposer.dll through
// GetProcAddress, so none of the SL_API declarations or header helpers is ever called.
#include <sl.h>
#include <sl_dlss.h>
#include <sl_security.h>

#include "backend.h"
#include "device.h"
#include "frame.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] dlss:";
constexpr uint32_t kVendorNvidia = 0x10DE;
// Warnings and errors of Streamline that go to the game's log (its information lines only go to its
// own log file, sl.log).
constexpr uint32_t kMaxSlLogLines = 200;
// This project's own id for Streamline and NGX: DLSS needs no NVIDIA application id (the
// engine is "custom"; NVIDIA RTX SDKs licence, Programming Guide 2.2 and 5.2.1).
constexpr const char* kProjectId = "ee04879d-28bc-46b8-a12d-8a0bdff650dc";
constexpr const char* kEngineVersion = "1.0.0";

// The files the game folder carries (cmake/FetchStreamline.cmake copies them next to the exe).
const wchar_t* const kFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll",
                                 L"nvngx_dlss.dll"};
// The three Streamline files whose signature is checked before anything is loaded (Streamline's
// own check: Windows trusts the file and the signer's key is NVIDIA's). nvngx_dlss.dll is loaded
// by sl.dlss.dll, which checks it itself.
const wchar_t* const kSignedFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll"};

std::string Utf8(const wchar_t* text) {
  if (!text) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) {
    return {};
  }
  std::string out(size_t(bytes) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

// The folder of the running exe, with a trailing separator; empty when it cannot be had.
std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH * 2];
  const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (length == 0 || length >= std::size(path)) {
    return {};
  }
  std::wstring dir(path, length);
  const size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

// Where LARecomp keeps its logs: next to the file the logger writes (LARECOMP_LOG_FILE), else
// ./logs (InitLarecompLogging; the same rule as LogFolder in bootprogress/larecomp_shader_build.cpp).
// Streamline's full log (sl.log: every line it writes, with NGX's) goes into it, which is what a
// bug report about DLSS needs. Empty, and no log file, when the folder cannot be had.
std::wstring StreamlineLogFolder() {
  std::error_code ec;
  std::filesystem::path folder;
  if (const char* file = std::getenv("LARECOMP_LOG_FILE"); file && *file) {
    const std::filesystem::path parent = std::filesystem::path(file).parent_path();
    if (!parent.empty()) {
      folder = std::filesystem::absolute(parent, ec);
    }
  }
  if (folder.empty()) {
    folder = std::filesystem::absolute("logs", ec);
  }
  if (ec || folder.empty()) {
    return {};
  }
  std::filesystem::create_directories(folder, ec);
  return ec ? std::wstring() : folder.wstring();
}

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A DLL by its full path: its dependencies are looked for in its own folder and in the system
// folder, never in the working directory or on PATH.
HMODULE LoadByFullPath(const std::wstring& path, DWORD* error) {
  HMODULE module = LoadLibraryExW(
      path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  *error = module ? 0 : GetLastError();
  return module;
}

// The names (UTF-8, comma separated) of the DLSS files that are not in `dir`; empty when all are.
std::string MissingFiles(const std::wstring& dir) {
  std::string missing;
  for (const wchar_t* file : kFiles) {
    if (dir.empty() || !FileExists(dir + file)) {
      missing += (missing.empty() ? "" : ", ") + Utf8(file);
    }
  }
  return missing;
}

// CHECK: that sl_security.h compiles with llvm-mingw too (it loads wintrust.dll and crypt32.dll by
// name, so no import library is needed), and that nvngx_dlss.dll, which sl.dlss.dll loads, is
// signature-checked by Streamline itself (its source says "secureLoadLibrary"; not read).
bool SignaturesOk(const std::wstring& dir, std::string* why) {
  for (const wchar_t* file : kSignedFiles) {
    const std::wstring path = dir + file;
    if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
      *why = std::format("{} does not carry NVIDIA's signature, or Windows does not trust it",
                         Utf8(file));
      return false;
    }
  }
  return true;
}

// Only to decide whether a card that cannot be asked (the files are not there, or the runtime is
// not loaded) is worth listing: every GeForce RTX, Quadro RTX, RTX A-series and Titan RTX has it in
// its name. The runtime's own query (slIsFeatureSupported) is the final word whenever it can run.
// CHECK: a future card name without "RTX" is only found when DLSS is the saved choice (the query is
// then asked at the start); the menu would not list it before.
bool NameLooksLikeRtx(const std::string& name) {
  for (size_t i = 0; i + 3 <= name.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(name[i])) == 'R' &&
        std::toupper(static_cast<unsigned char>(name[i + 1])) == 'T' &&
        std::toupper(static_cast<unsigned char>(name[i + 2])) == 'X') {
      return true;
    }
  }
  return false;
}

std::string ResultText(sl::Result result) {
  const char* name = "";
  switch (result) {
    case sl::Result::eOk: name = "ok"; break;
    case sl::Result::eErrorOSOutOfDate: name = "Windows is too old for it"; break;
    case sl::Result::eErrorDriverOutOfDate: name = "the graphics driver is too old for it"; break;
    case sl::Result::eErrorNoSupportedAdapterFound:
      name = "no graphics card that runs it (DLSS needs an NVIDIA RTX card)";
      break;
    case sl::Result::eErrorAdapterNotSupported: name = "this graphics card does not run it"; break;
    default: break;
  }
  return std::format("{}{}sl::Result {}", name, name[0] ? ", " : "", uint32_t(result));
}

// Streamline's own log lines, from whatever thread it is on. Only its warnings and errors go to the
// game's log (its information lines, a few hundred at every start, are all in sl.log: InitLocked
// gives it the logs folder), and no more than kMaxSlLogLines of them, so that a message it repeats
// cannot flood the log.
void SlLog(sl::LogType type, const char* message) {
  static std::atomic<uint32_t> lines{0};
  if (!message || type == sl::LogType::eInfo) {
    return;
  }
  const uint32_t count = lines.fetch_add(1, std::memory_order_relaxed);
  if (count > kMaxSlLogLines) {
    return;
  }
  if (count == kMaxSlLogLines) {
    REXLOG_WARN("{} streamline: more than {} warnings and errors: the rest are only in its own log "
                "(sl.log, in the logs folder)",
                kPrefix, kMaxSlLogLines);
    return;
  }
  std::string text(message);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  if (type == sl::LogType::eError) {
    REXLOG_ERROR("{} streamline: {}", kPrefix, text);
  } else {
    REXLOG_WARN("{} streamline: {}", kPrefix, text);
  }
}

sl::DLSSMode SlMode(UpscaleMode mode) {
  switch (mode) {
    case UpscaleMode::kQuality: return sl::DLSSMode::eMaxQuality;
    case UpscaleMode::kBalanced: return sl::DLSSMode::eBalanced;
    case UpscaleMode::kPerformance: return sl::DLSSMode::eMaxPerformance;
    case UpscaleMode::kUltraPerformance: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eDLAA;
  }
}

template <typename Fn>
Fn* Import(HMODULE module, const char* name) {
  return reinterpret_cast<Fn*>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// The functions of sl.interposer.dll this backend calls.
struct SlApi {
  PFun_slInit* init = nullptr;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slIsFeatureSupported* is_supported = nullptr;
  PFun_slGetFeatureFunction* get_function = nullptr;
  PFun_slGetNewFrameToken* new_frame_token = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
  PFun_slEvaluateFeature* evaluate = nullptr;
  PFun_slFreeResources* free_resources = nullptr;
  PFun_slSetD3DDevice* set_d3d_device = nullptr;
  // Taken through slGetFeatureFunction once the device is known.
  PFun_slDLSSGetOptimalSettings* dlss_optimal = nullptr;
  PFun_slDLSSSetOptions* dlss_set_options = nullptr;
  PFun_slDLSSGetState* dlss_state = nullptr;
};

// Streamline, once per process: the module, its start-up and every call (its API is not thread
// safe: `mu` serialises them). Shut down once, at the end of the run; it is not started again.
struct Runtime {
  std::mutex mu;
  HMODULE interposer = nullptr;
  SlApi api;
  bool loaded = false;        // the files are there and signed, the module is open, the functions found
  bool initialized = false;   // slInit succeeded
  bool shut_down = false;     // slShutdown ran, or the Vulkan hook was given up: off for the process
  bool files_problem = false; // why it failed: the files are missing, unsigned or would not load
  std::string failure;        // non-empty: Streamline is off for the process, and why
  uint32_t last_viewport = 0; // viewport ids are never reused (a context's resources are freed late)
  void* vk_get_instance_proc_addr = nullptr;  // the interposer's, when started for Vulkan
  bool vk_hook = false;                       // Plume is to load Vulkan through it
};

Runtime& Rt() {
  static Runtime runtime;
  return runtime;
}

bool LoadRuntimeLocked(Runtime& rt, std::string* why) {
  if (rt.loaded) {
    return true;
  }
  const std::wstring dir = ExeDirectory();
  const std::string missing = MissingFiles(dir);
  if (!missing.empty()) {
    *why = "these files are not next to the game's exe: " + missing;
    rt.files_problem = true;
    return false;
  }
  if (!SignaturesOk(dir, why)) {
    rt.files_problem = true;
    return false;
  }
  DWORD error = 0;
  rt.interposer = LoadByFullPath(dir + L"sl.interposer.dll", &error);
  if (!rt.interposer) {
    *why = std::format("sl.interposer.dll could not be opened (Windows error {})", error);
    rt.files_problem = true;
    return false;
  }
  SlApi& a = rt.api;
  a.init = Import<PFun_slInit>(rt.interposer, "slInit");
  a.shutdown = Import<PFun_slShutdown>(rt.interposer, "slShutdown");
  a.is_supported = Import<PFun_slIsFeatureSupported>(rt.interposer, "slIsFeatureSupported");
  a.get_function = Import<PFun_slGetFeatureFunction>(rt.interposer, "slGetFeatureFunction");
  a.new_frame_token = Import<PFun_slGetNewFrameToken>(rt.interposer, "slGetNewFrameToken");
  a.set_constants = Import<PFun_slSetConstants>(rt.interposer, "slSetConstants");
  a.evaluate = Import<PFun_slEvaluateFeature>(rt.interposer, "slEvaluateFeature");
  a.free_resources = Import<PFun_slFreeResources>(rt.interposer, "slFreeResources");
  a.set_d3d_device = Import<PFun_slSetD3DDevice>(rt.interposer, "slSetD3DDevice");
  if (!a.init || !a.shutdown || !a.is_supported || !a.get_function || !a.new_frame_token ||
      !a.set_constants || !a.evaluate || !a.free_resources || !a.set_d3d_device) {
    *why = "sl.interposer.dll lacks functions of Streamline's API";
    rt.files_problem = true;
    return false;
  }
  rt.loaded = true;
  return true;
}

// slInit, in the manual hooking mode: no proxies of the D3D12 or DXGI interfaces (Super Resolution
// needs none), the game's own device is given later. Not the defaults: no over-the-air plugin
// updates and no downloaded plugins (the files shipped with the game are the ones that run), the
// host (Plume's InvalidateAfterExternalWork) restores the command list state.
//
// Streamline's log: every line goes to sl.log in the logs folder (what a bug report needs); the
// callback hands the game's log only the warnings and errors (SlLog).
bool InitLocked(Runtime& rt, sl::RenderAPI api, std::string* why) {
  if (rt.initialized) {
    return true;
  }
  const std::wstring dir = ExeDirectory();
  const wchar_t* plugin_paths[] = {dir.c_str()};
  const sl::Feature features[] = {sl::kFeatureDLSS};
  const std::wstring logs = StreamlineLogFolder();
  sl::Preferences pref;
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = plugin_paths;
  pref.numPathsToPlugins = 1;
  pref.pathToLogsAndData = logs.empty() ? nullptr : logs.c_str();
  pref.logMessageCallback = &SlLog;
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = 1;
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = kEngineVersion;
  pref.projectId = kProjectId;
  pref.renderAPI = api;
  if (logs.empty()) {
    REXLOG_WARN("{} Streamline's own log file cannot be written (the logs folder is not usable): "
                "only its warnings and errors reach this log",
                kPrefix);
  } else {
    REXLOG_INFO("{} Streamline starts; its full log is {}\\sl.log (only its warnings and errors "
                "come into this log)",
                kPrefix, Utf8(logs.c_str()));
  }
  const sl::Result result = rt.api.init(pref, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    *why = "slInit failed: " + ResultText(result);
    return false;
  }
  rt.initialized = true;
  return true;
}

sl::float4x4 ToSl(const float* m) {
  sl::float4x4 out;
  for (uint32_t r = 0; r < 4; ++r) {
    out.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
  }
  return out;
}

// CHECK: the names and constructors below (sl::Resource(type, native, memory, view, state),
// ResourceTag(resource, type, lifecycle, extent*), kBufferTypeMotionVectors, DLSSOptions' fields)
// were read from Streamline 2.14.16's headers through a web page, not compiled: the first build
// settles them. Vulkan: the view must be a whole-image view that can be used for the image's usage
// flags, `memory` is passed but may be unnecessary.
sl::Resource MakeSlResource(const TemporalImage& image, const backend::NativeImage& native) {
  sl::Resource resource(sl::ResourceType::eTex2d, native.resource, native.memory, native.view,
                        native.state);
  resource.width = image.width;
  resource.height = image.height;
  resource.nativeFormat = native.native_format;
  resource.mipLevels = native.mip_levels;
  resource.arrayLayers = native.array_layers;
  resource.flags = native.create_flags;
  resource.usage = native.usage;
  return resource;
}

sl::Extent MakeExtent(uint32_t width, uint32_t height) {
  sl::Extent extent;
  extent.left = 0;
  extent.top = 0;
  extent.width = width;
  extent.height = height;
  return extent;
}

// One context: a viewport of its own (its resources are freed after the frames that may use them,
// by which time the next context has started, so the ids are never reused) and the sizes and mode
// its options were set for.
struct Context {
  BackendRequest request;
  uint32_t viewport_id = 0;
  bool dispatched = false;  // the first dispatch resets the history
};

}  // namespace

struct DlssBackend::Impl {
  // ---- set by Probe
  Device* device = nullptr;
  FrameRing* frames = nullptr;
  Backend api = Backend::kD3D12;
  backend::NativeDevice native;
  uint32_t vendor = 0;
  std::string gpu_name;
  bool probe_started = false;

  // ---- shared (under `mu`)
  mutable std::mutex mu;
  std::thread worker;
  BackendStatus status;
  bool probed = false;          // the answer the menu reads is final
  bool needs_setup = false;     // an RTX card whose DLSS files are missing or unusable
  bool after_restart = false;   // Vulkan: it runs from a start with DLSS chosen
  bool init_done = false;       // the start-up (load, slInit, device, support) succeeded
  std::shared_ptr<Context> ctx; // the ready context

  // ---- render thread only
  uint32_t frame_counter = 0;   // Streamline's frame index: its own count, always rising
  uint64_t dispatches = 0;

  void PublishLocked() {
    PublishDlssAvailability(probed, status.dlss && !status.failed, needs_setup, after_restart);
  }
  void StartWorkerLocked();
  void WorkerMain();
  bool StartRuntime(std::string* why, bool* files_problem);
  void FailStart(const std::string& why, bool files_problem);
  void FailLocked(std::unique_lock<std::mutex>& lock, const std::string& why);
  std::shared_ptr<Context> MakeContext(const BackendRequest& request, std::string* why);
  void FreeContext(const Context& context);
  void RetireContext(std::shared_ptr<Context> context);
};

DlssBackend::DlssBackend() : impl_(std::make_unique<Impl>()) {}

DlssBackend::~DlssBackend() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// the probe and the start-up
// ---------------------------------------------------------------------------

void DlssBackend::Impl::StartWorkerLocked() {
  if (!worker.joinable()) {
    worker = std::thread([this] { WorkerMain(); });
  }
}

void DlssBackend::Probe(Device& device, FrameRing& frames) {
  Impl& i = *impl_;
  if (i.probe_started) {
    return;
  }
  i.probe_started = true;
  i.device = &device;
  i.frames = &frames;
  i.api = ActiveBackend();
  i.native = backend::NativeDeviceOf(device.device());
  i.vendor = uint32_t(device.description().vendor);
  i.gpu_name = device.description().name;
  const bool rtx_name = NameLooksLikeRtx(i.gpu_name);
  const bool chosen = ReadUpscalerSettings().upscaler == Upscaler::kDlss;

  // A final answer for the menu: `dlss` runs now; `needs_setup` shows the row greyed (an RTX card
  // whose files are not usable); `after_restart` lists it for the next start (Vulkan).
  const auto settle = [&i](BackendState state, const std::string& reason, bool dlss,
                           bool needs_setup, bool after_restart) {
    std::lock_guard lock(i.mu);
    i.status.state = state;
    i.status.dlss = dlss;
    i.status.reason = reason;
    i.probed = true;
    i.needs_setup = needs_setup;
    i.after_restart = after_restart;
    i.PublishLocked();
  };

  if (i.vendor != kVendorNvidia) {
    REXLOG_INFO("{} not offered: '{}' is not an NVIDIA graphics card", kPrefix, i.gpu_name);
    settle(BackendState::kUnavailable, "this is not an NVIDIA graphics card", false, false, false);
    return;
  }

  // What an earlier start of Streamline in this process left (the Vulkan start before the device).
  bool runtime_off = false;
  bool runtime_files_problem = false;
  bool vulkan_hooked = false;
  std::string runtime_why;
  {
    Runtime& rt = Rt();
    std::lock_guard lock(rt.mu);
    runtime_off = rt.shut_down || !rt.failure.empty();
    runtime_files_problem = rt.files_problem;
    vulkan_hooked = rt.vk_hook;
    runtime_why = !rt.failure.empty() ? rt.failure : std::string("Streamline was shut down earlier in this run");
  }
  const std::string missing = MissingFiles(ExeDirectory());
  if (!missing.empty() || (runtime_off && runtime_files_problem)) {
    // The files cannot be used. An RTX card lists the row greyed as "needs setup"; no other
    // card lists anything (owner, F4-DESIGN.md 13 and 14).
    const std::string why =
        !missing.empty() ? "these files are not next to the game's exe: " + missing : runtime_why;
    if (rtx_name) {
      REXLOG_WARN("{} DLSS needs setup: {}. Run the game's setup again to get its files (the game "
                  "runs without it)",
                  kPrefix, why);
    } else {
      REXLOG_INFO("{} not offered: {}", kPrefix, why);
    }
    settle(BackendState::kUnavailable, why, false, rtx_name, false);
    return;
  }
  if (runtime_off) {
    REXLOG_WARN("{} DLSS is off for this session: {}", kPrefix, runtime_why);
    settle(BackendState::kUnavailable, runtime_why, false, false, false);
    return;
  }

  if (i.api == Backend::kVulkan) {
    if (!vulkan_hooked) {
      // The Vulkan device is made with Streamline's help, so DLSS has to be the choice when the
      // game starts (Device::Open, dlss::BeforeDevice).
#if defined(LARECOMP_PLUME_STREAMLINE)
      if (rtx_name) {
        REXLOG_INFO("{} listed for Vulkan, but this game was started without it: choosing DLSS "
                    "applies from the next start (the Vulkan device is made with Streamline)",
                    kPrefix);
        settle(BackendState::kUnavailable, "on Vulkan DLSS starts with the game", false, false, true);
      } else {
        REXLOG_INFO("{} not offered: '{}' is not recognised as an RTX card, and DLSS was not the "
                    "choice when the game started",
                    kPrefix, i.gpu_name);
        settle(BackendState::kUnavailable, "this is not an RTX card", false, false, false);
      }
#else
      REXLOG_INFO("{} not offered on Vulkan: this build's Plume has no creation hooks for "
                  "Streamline",
                  kPrefix);
      settle(BackendState::kUnavailable, "this build's Plume has no creation hooks", false, false,
             false);
#endif
      return;
    }
    // Started before the device: the worker finishes it (the support query).
    std::lock_guard lock(i.mu);
    i.status.state = BackendState::kProbing;
    i.StartWorkerLocked();
    return;
  }

  // D3D12: Streamline starts now when DLSS is the saved choice (the start's scene size needs the
  // answer), otherwise at its first use.
  if (chosen) {
    std::lock_guard lock(i.mu);
    i.status.state = BackendState::kProbing;
    i.StartWorkerLocked();
    return;
  }
  if (!rtx_name) {
    REXLOG_INFO("{} not offered: '{}' is not recognised as an RTX card", kPrefix, i.gpu_name);
    settle(BackendState::kUnavailable, "this is not an RTX card", false, false, false);
    return;
  }
  REXLOG_INFO("{} listed: '{}' is an RTX card and the files are there; Streamline starts when DLSS "
              "is chosen",
              kPrefix, i.gpu_name);
  settle(BackendState::kAvailable, {}, true, false, false);
}

void DlssBackend::Impl::WorkerMain() {
  std::string why;
  bool files_problem = false;
  const bool ok = StartRuntime(&why, &files_problem);
  if (!ok) {
    FailStart(why, files_problem);
    return;
  }
  {
    std::lock_guard lock(mu);
    init_done = true;
    status.state = BackendState::kAvailable;
    status.dlss = true;
    status.failed = false;
    status.reason.clear();
    status.provider = "Streamline sl.dlss";
    probed = true;
    needs_setup = false;
    after_restart = false;
    PublishLocked();
  }
  REXLOG_INFO("{} ready: DLSS can run on '{}' ({})", kPrefix, gpu_name, BackendName(api));
}

// Streamline's start-up for this device, on the worker: the module (signature checked), slInit (on
// D3D12; Vulkan started before the device), the device, the support query, DLSS's functions.
bool DlssBackend::Impl::StartRuntime(std::string* why, bool* files_problem) {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  *files_problem = false;
  if (rt.shut_down || !rt.failure.empty()) {
    *why = !rt.failure.empty() ? rt.failure : std::string("Streamline was shut down earlier in this run");
    *files_problem = rt.files_problem;
    return false;
  }
  if (!LoadRuntimeLocked(rt, why)) {
    rt.failure = *why;
    *files_problem = rt.files_problem;
    return false;
  }
  if (!rt.initialized) {
    if (api == Backend::kVulkan) {
      *why = "on Vulkan Streamline has to start before the device is made";
      return false;
    }
    if (!InitLocked(rt, sl::RenderAPI::eD3D12, why)) {
      rt.failure = *why;
      return false;
    }
  }
  if (api == Backend::kD3D12) {
    // The manual hooking guide, "slSetD3DDevice": "When using D3D simply create your device and
    // call slSetD3DDevice". Super Resolution needs no proxy of the device, the factory or the
    // swap chain.
    // CHECK: that Super Resolution runs with the native device alone (no slUpgradeInterface, no
    // call of the common plugin's presentCommon): if Streamline asks for the per-frame present
    // call (memory growing across frames, or the first dispatch failing), the swap chain has to
    // go through Plume's D3D12InterfaceHooks.upgradeSwapChain.
    const sl::Result result = rt.api.set_d3d_device(native.device);
    if (result != sl::Result::eOk) {
      *why = "slSetD3DDevice failed: " + ResultText(result);
      return false;
    }
  }
  sl::AdapterInfo adapter{};
  uint8_t luid[sizeof(uint64_t)] = {};
  std::memcpy(luid, &native.adapter_luid, sizeof(luid));
  if (api == Backend::kD3D12) {
    adapter.deviceLUID = luid;
    adapter.deviceLUIDSizeInBytes = uint32_t(sizeof(luid));
  } else {
    adapter.vkPhysicalDevice =
        reinterpret_cast<decltype(adapter.vkPhysicalDevice)>(native.physical_device);
  }
  const sl::Result supported = rt.api.is_supported(sl::kFeatureDLSS, adapter);
  if (supported != sl::Result::eOk) {
    *why = "Streamline says DLSS cannot run on '" + gpu_name + "': " + ResultText(supported);
    return false;
  }
  // DLSS's own functions (valid once the device is set).
  const auto function_of = [&rt, why](const char* name) -> void* {
    void* function = nullptr;
    const sl::Result r = rt.api.get_function(sl::kFeatureDLSS, name, function);
    if (r != sl::Result::eOk || !function) {
      *why = std::format("sl.dlss.dll has no {}: {}", name, ResultText(r));
      return nullptr;
    }
    return function;
  };
  void* get_optimal = function_of("slDLSSGetOptimalSettings");
  void* set_options = get_optimal ? function_of("slDLSSSetOptions") : nullptr;
  void* get_state = set_options ? function_of("slDLSSGetState") : nullptr;
  if (!get_optimal || !set_options || !get_state) {
    return false;
  }
  rt.api.dlss_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(get_optimal);
  rt.api.dlss_set_options = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
  rt.api.dlss_state = reinterpret_cast<PFun_slDLSSGetState*>(get_state);
  return true;
}

void DlssBackend::Impl::FailStart(const std::string& why, bool files_problem) {
  bool listed_setup = false;
  {
    std::lock_guard lock(mu);
    status.state = BackendState::kUnavailable;
    status.failed = true;
    status.dlss = false;
    status.reason = why;
    probed = true;
    needs_setup = files_problem && NameLooksLikeRtx(gpu_name);
    after_restart = false;
    listed_setup = needs_setup;
    PublishLocked();
  }
  REXLOG_WARN("{} DLSS is off for this session: {}{}", kPrefix, why,
              listed_setup ? ". Run the game's setup again to get its files" : "");
}

// ---------------------------------------------------------------------------
// the render thread's calls
// ---------------------------------------------------------------------------

BackendStatus DlssBackend::Status() const {
  std::lock_guard lock(impl_->mu);
  return impl_->status;
}

void DlssBackend::Impl::FailLocked(std::unique_lock<std::mutex>& lock, const std::string& why) {
  // Under `mu`. One log line; DLSS is off for the session, the context goes after the frames that
  // may use it.
  const bool first = !status.failed;
  status.failed = true;
  status.state = BackendState::kUnavailable;
  status.dlss = false;
  status.reason = why;
  status.context_bytes = 0;
  std::shared_ptr<Context> retire = std::move(ctx);
  ctx.reset();
  PublishLocked();
  lock.unlock();
  if (first) {
    REXLOG_WARN("{} DLSS is off for this session: {}", kPrefix, why);
  }
  RetireContext(std::move(retire));
  lock.lock();
}

void DlssBackend::Impl::FreeContext(const Context& context) {
  // After the frames that may use it, so no dispatch of this viewport is pending on a list.
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (!rt.initialized || rt.shut_down || !rt.api.free_resources) {
    return;
  }
  rt.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(context.viewport_id));
}

void DlssBackend::Impl::RetireContext(std::shared_ptr<Context> context) {
  if (!context) {
    return;
  }
  if (!frames) {
    FreeContext(*context);
    return;
  }
  // Run on the render thread once every frame recorded so far has finished (the renderer drains
  // the ring before it shuts this backend down).
  frames->RetireAction([this, context] { FreeContext(*context); });
}

std::shared_ptr<Context> DlssBackend::Impl::MakeContext(const BackendRequest& request,
                                                        std::string* why) {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (!rt.initialized || rt.shut_down || !rt.api.dlss_optimal || !rt.api.dlss_set_options) {
    *why = "Streamline is not running";
    return {};
  }
  // NVIDIA's default presets (no preset row), the scene colour is linear HDR with no exposure of
  // its own (automatic exposure).
  sl::DLSSOptions options;
  options.mode = SlMode(request.mode);
  options.outputWidth = request.output_size[0];
  options.outputHeight = request.output_size[1];
  options.preExposure = 1.0f;
  options.exposureScale = 1.0f;
  options.colorBuffersHDR = sl::Boolean::eTrue;
  options.useAutoExposure = sl::Boolean::eTrue;
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;

  // CHECK: the meaning of renderWidthMin/Max: read as DLSS's dynamic resolution range; when the
  // runtime reports none (0) the scene must be within two pixels of the optimal size.
  // The size DLSS wants for this mode and output. The scene was drawn at the start's size (the
  // vendor's ratio over the render scale: temporal_backend.cpp kDlssModes), which can differ from
  // it by a few pixels, or by a mode picked since from another vendor's list: anything in DLSS's
  // dynamic range works.
  sl::DLSSOptimalSettings optimal;
  const sl::Result got = rt.api.dlss_optimal(options, optimal);
  if (got != sl::Result::eOk) {
    *why = "slDLSSGetOptimalSettings failed: " + ResultText(got);
    return {};
  }
  const uint32_t rw = request.render_size[0];
  const uint32_t rh = request.render_size[1];
  const auto near_to = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 2u; };
  const bool has_range = optimal.renderWidthMax != 0 && optimal.renderHeightMax != 0;
  const bool fits = has_range ? (rw >= optimal.renderWidthMin && rw <= optimal.renderWidthMax &&
                                 rh >= optimal.renderHeightMin && rh <= optimal.renderHeightMax)
                              : (near_to(rw, optimal.optimalRenderWidth) &&
                                 near_to(rh, optimal.optimalRenderHeight));
  if (!fits) {
    *why = std::format("the scene is drawn at {}x{}, which DLSS does not take for {}x{} in this "
                       "mode: it asks for {}x{} (range {}x{} to {}x{})",
                       rw, rh, request.output_size[0], request.output_size[1],
                       optimal.optimalRenderWidth, optimal.optimalRenderHeight,
                       optimal.renderWidthMin, optimal.renderHeightMin, optimal.renderWidthMax,
                       optimal.renderHeightMax);
    return {};
  }
  auto context = std::make_shared<Context>();
  context->request = request;
  context->viewport_id = ++rt.last_viewport;
  const sl::Result set = rt.api.dlss_set_options(sl::ViewportHandle(context->viewport_id), options);
  if (set != sl::Result::eOk) {
    *why = "slDLSSSetOptions failed: " + ResultText(set);
    return {};
  }
  REXLOG_INFO("{} context for {} at {}x{} to {}x{} (viewport {}): DLSS asks for {}x{} (range {}x{} to "
              "{}x{}); the feature itself is made at its first dispatch",
              kPrefix, ModeInfoOf(Upscaler::kDlss, request.mode).name, rw, rh,
              request.output_size[0], request.output_size[1], context->viewport_id,
              optimal.optimalRenderWidth, optimal.optimalRenderHeight, optimal.renderWidthMin,
              optimal.renderHeightMin, optimal.renderWidthMax, optimal.renderHeightMax);
  return context;
}

void DlssBackend::Request(const BackendRequest& request) {
  Impl& i = *impl_;
  std::shared_ptr<Context> retire;
  {
    std::unique_lock lock(i.mu);
    if (i.status.failed || i.status.state != BackendState::kAvailable) {
      return;
    }
    if (!i.init_done) {
      // The first use of a DLSS that was not the choice at the start (D3D12): Streamline starts
      // now, on the worker; no context until it has.
      i.StartWorkerLocked();
      return;
    }
    if (i.ctx && i.ctx->request == request) {
      return;
    }
    if (i.ctx) {
      retire = std::move(i.ctx);
      i.ctx.reset();
      i.status.context_bytes = 0;
    }
    lock.unlock();
    std::string why;
    std::shared_ptr<Context> made = i.MakeContext(request, &why);
    lock.lock();
    if (!made) {
      i.FailLocked(lock, "its context could not be made: " + why);
    } else if (!i.status.failed) {
      i.ctx = std::move(made);
    }
  }
  i.RetireContext(std::move(retire));
}

bool DlssBackend::Ready(const BackendRequest& request) const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  return i.ctx && i.ctx->request == request && !i.status.failed;
}

void DlssBackend::Release() {
  Impl& i = *impl_;
  std::shared_ptr<Context> retire;
  {
    std::lock_guard lock(i.mu);
    retire = std::move(i.ctx);
    i.ctx.reset();
    i.status.context_bytes = 0;
  }
  i.RetireContext(std::move(retire));
}

void DlssBackend::Fail(const std::string& why) {
  Impl& i = *impl_;
  std::unique_lock lock(i.mu);
  i.FailLocked(lock, why);
}

bool DlssBackend::Upscale(plume::RenderCommandList* list, const TemporalInputs& in) {
  Impl& i = *impl_;
  std::shared_ptr<Context> context;
  {
    std::lock_guard lock(i.mu);
    context = i.ctx;
  }
  const auto fail = [this](const std::string& why) {
    Fail(why);
    return false;
  };
  if (!context) {
    return fail("a dispatch was asked for with no context");
  }
  void* native_list = backend::NativeListHandle(list);
  if (!native_list) {
    return fail("the command list has no native object");
  }
  const backend::NativeImage color_native =
      backend::NativeImageOf(in.color.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage depth_native =
      backend::NativeImageOf(in.depth.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage motion_native =
      backend::NativeImageOf(in.motion.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage output_native =
      backend::NativeImageOf(in.output.texture, backend::ImageUse::kStorage);
  if (!color_native.complete || !depth_native.complete || !motion_native.complete ||
      !output_native.complete) {
    return fail("an input image has no native object, or Plume cannot describe it to Streamline");
  }

  // The images and their tags, local to this evaluation (no global tags, so no frame-based tagging
  // flag and nothing that waits for a present). The states are the ones the caller transitioned
  // them to; Streamline takes them back to those.
  // CHECK: that Streamline leaves the four images in the states declared here (the caller's
  // barriers after the dispatch assume it, as they do for FidelityFX): the D3D12 debug layer with
  // GPU-based validation, and Vulkan validation with synchronization validation, over the first
  // frames. Local tags with the eValidUntilEvaluate lifecycle are also unverified.
  sl::Resource color_res = MakeSlResource(in.color, color_native);
  sl::Resource depth_res = MakeSlResource(in.depth, depth_native);
  sl::Resource motion_res = MakeSlResource(in.motion, motion_native);
  sl::Resource output_res = MakeSlResource(in.output, output_native);
  const sl::Extent render_extent = MakeExtent(in.render_size[0], in.render_size[1]);
  const sl::Extent output_extent = MakeExtent(in.output_size[0], in.output_size[1]);
  const sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate;
  sl::ResourceTag color_tag(&color_res, sl::kBufferTypeScalingInputColor, lifecycle, &render_extent);
  sl::ResourceTag depth_tag(&depth_res, sl::kBufferTypeDepth, lifecycle, &render_extent);
  sl::ResourceTag motion_tag(&motion_res, sl::kBufferTypeMotionVectors, lifecycle, &render_extent);
  sl::ResourceTag output_tag(&output_res, sl::kBufferTypeScalingOutputColor, lifecycle,
                             &output_extent);

  // The constants (row-major matrices of row vectors without the jitter, as the game's own are).
  const bool reset = in.reset || !context->dispatched;
  sl::Constants consts;
  consts.cameraViewToClip = ToSl(in.view_to_clip);
  consts.clipToCameraView = ToSl(in.clip_to_view);
  consts.clipToPrevClip = ToSl(in.clip_to_prev_clip);
  consts.prevClipToClip = ToSl(in.prev_clip_to_clip);
  // CHECK: the sign of the jitter. What the draws were moved by, in render pixels, +x right and +y
  // down, the convention FSR takes; DLSS's documentation says "pixel space" only. If the picture
  // is soft or shimmers, flip both here (the converged-sharpness test of F4-DESIGN.md 9.3).
  consts.jitterOffset = sl::float2(in.jitter[0], in.jitter[1]);
  // CHECK: the vectors are previous minus current in render pixels (+y down), which scaled by one
  // over the render size are in [-1, 1] as Streamline asks; if DLSS ghosts along the motion, the
  // direction or the scale is the first suspect.
  consts.mvecScale = sl::float2(in.motion_scale[0] / float(in.render_size[0]),
                                in.motion_scale[1] / float(in.render_size[1]));
  consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);  // the camera has none (not left invalid)
  consts.cameraPos = sl::float3(in.camera_position[0], in.camera_position[1], in.camera_position[2]);
  consts.cameraUp = sl::float3(in.camera_up[0], in.camera_up[1], in.camera_up[2]);
  consts.cameraRight = sl::float3(in.camera_right[0], in.camera_right[1], in.camera_right[2]);
  consts.cameraFwd = sl::float3(in.camera_forward[0], in.camera_forward[1], in.camera_forward[2]);
  consts.cameraNear = in.camera_near;
  consts.cameraFar = in.camera_far;
  consts.cameraFOV = in.camera_fov_y;
  consts.cameraAspectRatio = in.camera_aspect;
  consts.depthInverted = sl::Boolean::eTrue;           // the game stores 1 near, 0 far
  consts.cameraMotionIncluded = sl::Boolean::eTrue;    // the vectors are the camera's motion
  consts.motionVectors3D = sl::Boolean::eFalse;
  consts.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  consts.orthographicProjection = sl::Boolean::eFalse;
  consts.motionVectorsDilated = sl::Boolean::eFalse;
  consts.motionVectorsJittered = sl::Boolean::eFalse;  // the jitter is taken out of them

  std::string error;
  uint64_t estimated_bytes = 0;
  {
    Runtime& rt = Rt();
    std::lock_guard sl_lock(rt.mu);
    if (!rt.initialized || rt.shut_down) {
      error = "Streamline is not running";
    } else {
      sl::FrameToken* token = nullptr;
      uint32_t frame_index = ++i.frame_counter;
      sl::Result result = rt.api.new_frame_token(token, &frame_index);
      if (result != sl::Result::eOk || !token) {
        error = "slGetNewFrameToken failed: " + ResultText(result);
      } else {
        const sl::ViewportHandle viewport(context->viewport_id);
        result = rt.api.set_constants(consts, *token, viewport);
        if (result != sl::Result::eOk) {
          error = "slSetConstants failed: " + ResultText(result);
        } else {
          const sl::BaseStructure* inputs[] = {&viewport, &color_tag, &depth_tag, &motion_tag,
                                               &output_tag};
          result = rt.api.evaluate(sl::kFeatureDLSS, *token, inputs, uint32_t(std::size(inputs)),
                                   native_list);
          if (result != sl::Result::eOk) {
            error = "slEvaluateFeature failed: " + ResultText(result);
          } else if (!context->dispatched && rt.api.dlss_state) {
            sl::DLSSState state;
            if (rt.api.dlss_state(viewport, state) == sl::Result::eOk) {
              estimated_bytes = state.estimatedVRAMUsageInBytes;
            }
          }
        }
      }
    }
  }
  if (!error.empty()) {
    return fail(error);
  }
  if (!context->dispatched) {
    context->dispatched = true;
    std::lock_guard lock(i.mu);
    i.status.context_bytes = estimated_bytes;
  }
  if (i.dispatches++ == 0) {
    REXLOG_INFO("{} first dispatch recorded: {}x{} to {}x{}, jitter {:+.3f}, {:+.3f}, near {:.3f}, "
                "far {:.1f}, field of view {:.2f} deg, reset {}, about {} MB",
                kPrefix, in.render_size[0], in.render_size[1], in.output_size[0], in.output_size[1],
                in.jitter[0], in.jitter[1], in.camera_near, in.camera_far,
                double(in.camera_fov_y) * 57.29577951308232, reset ? "yes" : "no",
                estimated_bytes >> 20);
  }
  return true;
}

void DlssBackend::Shutdown() {
  Impl& i = *impl_;
  // The start-up on the worker is short; the device is still alive, so wait for it.
  if (i.worker.joinable()) {
    i.worker.join();
  }
  std::shared_ptr<Context> last;
  {
    std::lock_guard lock(i.mu);
    last = std::move(i.ctx);
    i.ctx.reset();
  }
  if (i.probe_started) {
    // The GPU is idle (the renderer drained its frames): the resources go, then Streamline, before
    // the device does ("Call slShutdown() before destroying those instances"). The module stays
    // loaded to the end of the process.
    Runtime& rt = Rt();
    std::lock_guard lock(rt.mu);
    if (rt.initialized && !rt.shut_down) {
      if (last) {
        rt.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(last->viewport_id));
      }
      rt.api.shutdown();
      rt.initialized = false;
      rt.shut_down = true;
    }
  }
  i.frames = nullptr;
  i.probe_started = false;
}

std::unique_ptr<TemporalBackend> MakeDlssBackend() {
  return std::make_unique<DlssBackend>();
}

// ---------------------------------------------------------------------------
// Vulkan: Streamline before the device
// ---------------------------------------------------------------------------

namespace dlss {

void BeforeDevice(Backend backend, const std::string& preferred_adapter) {
  if (backend != Backend::kVulkan) {
    (void)preferred_adapter;
    return;
  }
  if (ReadUpscalerSettings().upscaler != Upscaler::kDlss) {
    return;
  }
#if !defined(LARECOMP_PLUME_STREAMLINE)
  REXLOG_INFO("{} DLSS is chosen, but this build's Plume has no creation hooks for Streamline: "
              "it is not available on Vulkan in this build",
              kPrefix);
  (void)preferred_adapter;
#else
  // CHECK: that Streamline's vkCreateDevice proxy accepts Plume's device chain (individual feature
  // structs such as VkPhysicalDeviceDescriptorIndexingFeatures and pEnabledFeatures): it adds
  // VkPhysicalDeviceVulkan12Features of its own, which the specification does not allow next to the
  // individual ones. If the device cannot be made the game starts again without DLSS (Device); the
  // fix would be in Plume (Vulkan12Features instead of the individual structs) or a vkCreateDevice
  // wrapper of ours that merges.
  const uint32_t vendor = backend::RenderingAdapterVendor(preferred_adapter);
  if (vendor != kVendorNvidia) {
    REXLOG_INFO("{} DLSS is chosen, but the Vulkan device will not be an NVIDIA one (vendor "
                "{:#06x}): Streamline is not started",
                kPrefix, vendor);
    return;
  }
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (rt.shut_down || !rt.failure.empty()) {
    return;
  }
  std::string why;
  if (!LoadRuntimeLocked(rt, &why) || !InitLocked(rt, sl::RenderAPI::eVulkan, &why)) {
    // The probe reports it (greyed as "needs setup" when the files are the problem).
    rt.failure = why;
    REXLOG_INFO("{} Streamline is not started for Vulkan: {}", kPrefix, why);
    return;
  }
  void* get_instance_proc_addr =
      reinterpret_cast<void*>(GetProcAddress(rt.interposer, "vkGetInstanceProcAddr"));
  if (!get_instance_proc_addr) {
    rt.failure = "sl.interposer.dll does not export vkGetInstanceProcAddr";
    REXLOG_INFO("{} Streamline is not started for Vulkan: {}", kPrefix, rt.failure);
    return;
  }
  rt.vk_get_instance_proc_addr = get_instance_proc_addr;
  rt.vk_hook = true;
  REXLOG_INFO("{} Streamline started for Vulkan before the device: Plume loads Vulkan through its "
              "vkGetInstanceProcAddr, whose vkCreateInstance and vkCreateDevice add what DLSS "
              "needs",
              kPrefix);
#endif
}

void* VulkanInstanceProcAddr() {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  return rt.vk_hook ? rt.vk_get_instance_proc_addr : nullptr;
}

void AbandonVulkanHook(const std::string& why) {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (!rt.vk_hook) {
    return;
  }
  rt.vk_hook = false;
  rt.failure = why;
  if (rt.initialized && !rt.shut_down) {
    rt.api.shutdown();
    rt.initialized = false;
  }
  rt.shut_down = true;
  REXLOG_WARN("{} DLSS is off for this session: {}", kPrefix, why);
}

}  // namespace dlss

}  // namespace mcla::gpu
