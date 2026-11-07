// MCLA GPU renderer -- the DLSS (Streamline) upscaler backend. See temporal_dlss.h.

#include "temporal_dlss.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
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
#if defined(LARECOMP_HAS_DLSS_G)
// DLSS frame generation, and Reflex with the PC latency markers, which it requires.
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif
#if defined(__MINGW32__)
// Streamline's sl_security.h counts a file's nested signatures with WINTRUST_SIGNATURE_SETTINGS
// (Windows 8's wintrust), which mingw-w64's headers do not declare: VerifiedByNvidia below.
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#else
#include <sl_security.h>
#endif

#include "backend.h"
#include "device.h"
#include "frame.h"
#include "latency.h"
#include "texture_bytes.h"
#include "vram_budget.h"

REXCVAR_DECLARE(std::string, mcla_gpu_frame_generation);  // frame_generation.cpp

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
// A pre-warm step (Prewarm) makes one feature and frees it at once. What it may take of video memory
// besides its four images: the feature (47 to 100 MB at 1280x720, more at 4K), with room to spare.
constexpr uint64_t kPrewarmFeatureReserveBytes = 256ull << 20;

// The images of the DLSS dispatch, as the temporal system makes them (temporal.cpp RecordUpscale,
// temporal_mv.cpp): the warm-up (Warmup) makes the same ones, once, to make the feature in the
// background.
constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::R32_FLOAT;
constexpr plume::RenderFormat kMotionFormat = plume::RenderFormat::R16G16_FLOAT;
constexpr plume::RenderTextureFlags kColorFlags = plume::RenderTextureFlag::NONE;
constexpr plume::RenderTextureFlags kDepthMotionFlags = plume::RenderTextureFlag::RENDER_TARGET;
constexpr plume::RenderTextureFlags kOutputFlags =
    plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

// The files the game folder carries (cmake/FetchStreamline.cmake copies them next to the exe).
const wchar_t* const kFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll",
                                 L"nvngx_dlss.dll"};
// The three Streamline files whose signature is checked before anything is loaded (Streamline's
// own check: Windows trusts the file and the signer's key is NVIDIA's). nvngx_dlss.dll is loaded
// by sl.dlss.dll, which checks it itself.
const wchar_t* const kSignedFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll"};
// Frame generation's (dlss_frame_generation.h): the plugin and its network, and the two plugins it
// requires. Needed only when Streamline is started with them (InitLocked); the three plugins are
// checked like the others, nvngx_dlssg.dll is loaded and checked by sl.dlss_g.dll.
const wchar_t* const kFrameGenFiles[] = {L"sl.dlss_g.dll", L"nvngx_dlssg.dll", L"sl.reflex.dll",
                                         L"sl.pcl.dll"};
const wchar_t* const kFrameGenSignedFiles[] = {L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll"};

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

// Whether Windows trusts the file's signature and NVIDIA signed it. With the Clang/MSVC build this is
// Streamline's own check (sl_security.h: the primary signature trusted by Windows, and a nested one made
// with Streamline's pinned NVIDIA key). mingw-w64's wintrust.h lacks what the nested part needs, so
// that build checks the primary signature: trusted by Windows (WinVerifyTrust) and its signer's name is
// NVIDIA Corporation, which is what all four files carry. Both load wintrust.dll and crypt32.dll by
// name: no import library either way.
bool VerifiedByNvidia(const std::wstring& path) {
#if defined(__MINGW32__)
  HMODULE wintrust = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE crypt32 = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!wintrust || !crypt32) {
    return false;
  }
  using VerifyFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
  using ProviderFn = CRYPT_PROVIDER_DATA*(WINAPI*)(HANDLE);
  using SignerFn = CRYPT_PROVIDER_SGNR*(WINAPI*)(CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD);
  using CertFn = CRYPT_PROVIDER_CERT*(WINAPI*)(CRYPT_PROVIDER_SGNR*, DWORD);
  using NameFn = DWORD(WINAPI*)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD);
  auto verify = reinterpret_cast<VerifyFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WinVerifyTrust")));
  auto provider_of = reinterpret_cast<ProviderFn>(
      reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperProvDataFromStateData")));
  auto signer_of = reinterpret_cast<SignerFn>(
      reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperGetProvSignerFromChain")));
  auto cert_of = reinterpret_cast<CertFn>(
      reinterpret_cast<void*>(GetProcAddress(wintrust, "WTHelperGetProvCertFromChain")));
  auto name_of = reinterpret_cast<NameFn>(reinterpret_cast<void*>(GetProcAddress(crypt32, "CertGetNameStringW")));
  if (!verify || !provider_of || !signer_of || !cert_of || !name_of) {
    return false;
  }
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const HWND no_ui = static_cast<HWND>(INVALID_HANDLE_VALUE);
  bool ok = verify(no_ui, &action, &data) == ERROR_SUCCESS;
  if (ok) {
    ok = false;
    if (CRYPT_PROVIDER_DATA* provider = provider_of(data.hWVTStateData)) {
      if (CRYPT_PROVIDER_SGNR* signer = signer_of(provider, 0, FALSE, 0)) {
        CRYPT_PROVIDER_CERT* leaf = cert_of(signer, 0);
        if (leaf && leaf->pCert) {
          wchar_t name[256] = {};
          name_of(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
          ok = std::wcscmp(name, L"NVIDIA Corporation") == 0;
        }
      }
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  verify(no_ui, &action, &data);
  return ok;
#else
  return sl::security::verifyEmbeddedSignature(path.c_str());
#endif
}

// nvngx_dlss.dll, which sl.dlss.dll loads, is checked by Streamline itself.
bool SignaturesOk(const std::wstring& dir, std::string* why) {
  for (const wchar_t* file : kSignedFiles) {
    const std::wstring path = dir + file;
    if (!VerifiedByNvidia(path)) {
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
// Limitation: a card whose name lacks "RTX" is only found when DLSS is the saved choice (Streamline's
// query runs at that start); the menu does not list it before.
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

// Whether Streamline is started with the frame generation plugins (InitLocked): where the player has
// chosen DLSS frame generation, and on D3D12 for every card that can run it (the choice may come
// while the game runs). On Vulkan only the saved choice at the start counts: the device is made
// through Streamline then, which costs seconds, and the plugins' extensions and queues go into it.
bool WantFrameGenerationPlugins(Backend backend, const std::string& gpu_name) {
  if (REXCVAR_GET(mcla_gpu_frame_generation) == "dlss") {
    return true;
  }
  // On D3D12 for every card that can run it by its name (plugins cannot be added after slInit, and
  // the choice may come while the game runs); the support query is the final word.
  return backend == Backend::kD3D12 && dlss::NameLooksLikeFrameGeneration(gpu_name);
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
    case sl::Result::eErrorOSDisabledHWS:
      name = "hardware-accelerated GPU scheduling is off (Windows Settings, System, Display, "
             "Graphics, Default graphics settings)";
      break;
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

// The options of a viewport for a mode and an output size, which are also what
// slDLSSGetOptimalSettings is asked with: NVIDIA's default presets (no preset row), the scene colour
// is linear HDR with no exposure of its own (automatic exposure).
sl::DLSSOptions OptionsFor(UpscaleMode mode, uint32_t output_width, uint32_t output_height) {
  sl::DLSSOptions options;
  options.mode = SlMode(mode);
  options.outputWidth = output_width;
  options.outputHeight = output_height;
  options.preExposure = 1.0f;
  options.exposureScale = 1.0f;
  options.colorBuffersHDR = sl::Boolean::eTrue;
  options.useAutoExposure = sl::Boolean::eTrue;
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  return options;
}

// Whether a scene of rw x rh is a size DLSS takes for the mode and output `optimal` was asked for. The
// renderWidthMin/Max range is DLSS's dynamic resolution range (measured on an RTX 5080: 1267x713 to
// 1280x720 for DLAA at 1280x720, 640x360 to 1280x720 for Quality there, 1920x1080 to 3840x2160 for
// Performance at 3840x2160, and exactly 1280x720 for Ultra Performance at 3840x2160). Without a range
// the size must be the optimal one, within two pixels.
bool SceneFits(const sl::DLSSOptimalSettings& optimal, uint32_t rw, uint32_t rh) {
  const auto near_to = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 2u; };
  const bool has_range = optimal.renderWidthMax != 0 && optimal.renderHeightMax != 0;
  return has_range ? (rw >= optimal.renderWidthMin && rw <= optimal.renderWidthMax &&
                      rh >= optimal.renderHeightMin && rh <= optimal.renderHeightMax)
                   : (near_to(rw, optimal.optimalRenderWidth) &&
                      near_to(rh, optimal.optimalRenderHeight));
}

// What DLSS takes for the mode and output `optimal` was asked for, against the scene's size.
std::string SceneWhy(const sl::DLSSOptimalSettings& optimal, uint32_t rw, uint32_t rh,
                     uint32_t output_width, uint32_t output_height) {
  const bool exact = optimal.renderWidthMin == optimal.renderWidthMax &&
                     optimal.renderHeightMin == optimal.renderHeightMax;
  if (exact) {
    return std::format("for {}x{} DLSS takes a scene of {}x{} in this mode, and the scene would be "
                       "drawn at {}x{}",
                       output_width, output_height, optimal.optimalRenderWidth,
                       optimal.optimalRenderHeight, rw, rh);
  }
  return std::format("for {}x{} DLSS takes a scene of {}x{} to {}x{} in this mode (best {}x{}), and "
                     "the scene would be drawn at {}x{}",
                     output_width, output_height, optimal.renderWidthMin, optimal.renderHeightMin,
                     optimal.renderWidthMax, optimal.renderHeightMax, optimal.optimalRenderWidth,
                     optimal.optimalRenderHeight, rw, rh);
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
  PFun_slUpgradeInterface* upgrade_interface = nullptr;  // optional: D3D12's swap chain wrapper
  // Optional: frame generation's (tags for a frame, a feature loaded and unloaded again).
  PFun_slSetTagForFrame* set_tag_for_frame = nullptr;
  PFun_slSetFeatureLoaded* set_feature_loaded = nullptr;
  // Taken through slGetFeatureFunction once the device is known.
  PFun_slDLSSGetOptimalSettings* dlss_optimal = nullptr;
  PFun_slDLSSSetOptions* dlss_set_options = nullptr;
  PFun_slDLSSGetState* dlss_state = nullptr;
#if defined(LARECOMP_HAS_DLSS_G)
  // The frame generation, Reflex and PC latency plugins' functions, taken through slGetFeatureFunction
  // (the frame generation ones again whenever the feature is loaded again).
  PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
  PFun_slDLSSGGetState* dlssg_get_state = nullptr;
  PFun_slReflexSetOptions* reflex_set_options = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
  PFun_slPCLSetMarker* pcl_set_marker = nullptr;
#endif
};

// Streamline, once per process: the module, its start-up and every call (its API is not thread
// safe: `mu` serialises them). Shut down once, at the end of the run; it is not started again.
//
// Lock order: Runtime::mu, then DlssBackend::Impl::mu (never the other way round). Nothing that
// holds Impl::mu waits for Runtime::mu, and the render thread never waits for it for long: the
// calls that can take seconds (the start-up and the creation of a feature) are made on the worker,
// and the render thread only calls Streamline while a ready context exists, which is only when the
// worker is not busy making one.
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
  bool swap_chain_tried = false;              // D3D12: the swap chain was offered to slUpgradeInterface
  // slInit was given the frame generation, Reflex and PC latency plugins (InitLocked), and the three
  // are loaded: the frame generation one is unloaded again after the support query and loaded while
  // it runs (dlss::LoadFrameGeneration), Reflex and PC latency stay.
  bool frame_gen_asked = false;
  bool frame_gen_loaded = false;  // the frame generation plugin is loaded now
};

// The frame generation facts the menu and the module read (dlss::FrameGenerationFacts): written by
// the worker with Runtime::mu held, read under their own lock so that a reader never waits for a
// feature's creation (seconds).
std::mutex g_fg_mutex;
dlss::FrameGenerationFacts g_fg_facts;
// The worker is making a feature (or Streamline's start): the render thread's Streamline calls that
// would wait for the lock say busy instead (Impl::building is this).
std::atomic<bool> g_building{false};
// The swap chain Plume presents on is Streamline's own proxy (frame generation made it through the
// proxy factory): no wrapper of ours goes over it, and a chain made again is plain until frame
// generation is on again.
std::atomic<bool> g_chain_proxied{false};
// Reflex and the PC latency markers can be called (the functions are there, Streamline runs): read by
// the update thread and the render thread without the lock; Reflex's sleep and the markers are
// thread safe in Streamline and must not wait behind a feature's creation.
std::atomic<bool> g_reflex_ready{false};

// D3D12: Streamline has fully started and the swap chain is still to be handed to it
// (dlss::UpgradeSwapChain). Read at every present, so not behind Runtime::mu.
std::atomic<bool> g_swap_chain_pending{false};
// D3D12: a swap chain was offered to Streamline once (taken or refused), and the chain was made
// again since (dlss::SwapChainRecreated: the HDR switch destroys the wrapped chain and makes a plain
// one): the next present offers the new one. Both read at every present.
std::atomic<bool> g_swap_chain_offered{false};
std::atomic<bool> g_swap_chain_again{false};

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
  a.upgrade_interface = Import<PFun_slUpgradeInterface>(rt.interposer, "slUpgradeInterface");
  a.set_tag_for_frame = Import<PFun_slSetTagForFrame>(rt.interposer, "slSetTagForFrame");
  a.set_feature_loaded = Import<PFun_slSetFeatureLoaded>(rt.interposer, "slSetFeatureLoaded");
  if (!a.init || !a.shutdown || !a.is_supported || !a.get_function || !a.new_frame_token ||
      !a.set_constants || !a.evaluate || !a.free_resources || !a.set_d3d_device) {
    *why = "sl.interposer.dll lacks functions of Streamline's API";
    rt.files_problem = true;
    return false;
  }
  rt.loaded = true;
  return true;
}

// Whether the frame generation files are there and the three plugins signed (`why` says what not).
bool FrameGenFilesOk(const std::wstring& dir, std::string* why) {
  std::string missing;
  for (const wchar_t* file : kFrameGenFiles) {
    if (dir.empty() || !FileExists(dir + file)) {
      missing += (missing.empty() ? "" : ", ") + Utf8(file);
    }
  }
  if (!missing.empty()) {
    *why = "these files are not next to the game's exe: " + missing;
    return false;
  }
  for (const wchar_t* file : kFrameGenSignedFiles) {
    if (!VerifiedByNvidia(dir + file)) {
      *why = std::format("{} does not carry NVIDIA's signature, or Windows does not trust it",
                         Utf8(file));
      return false;
    }
  }
  return true;
}

// slInit, in the manual hooking mode: no proxies of the D3D12 or DXGI interfaces (Super Resolution
// needs none), the game's own device is given later. Not the defaults: no over-the-air plugin
// updates and no downloaded plugins (the files shipped with the game are the ones that run), the
// host (Plume's InvalidateAfterExternalWork) restores the command list state.
//
// `frame_gen`: also the plugins of DLSS frame generation, Reflex and the PC latency markers (the
// build has them, their files are in place and signed): features can only be asked for here, so this
// is decided by who may use them (WantFrameGenerationPlugins). With them the tags are the frame
// based kind (slSetTagForFrame), which frame generation needs: Super Resolution's own tags are local
// to its evaluation and unaffected.
//
// Streamline's log: every line goes to sl.log in the logs folder (what a bug report needs); the
// callback hands the game's log only the warnings and errors (SlLog).
bool InitLocked(Runtime& rt, sl::RenderAPI api, bool frame_gen, std::string* why) {
  if (rt.initialized) {
    return true;
  }
  const std::wstring dir = ExeDirectory();
  const wchar_t* plugin_paths[] = {dir.c_str()};
  std::vector<sl::Feature> features = {sl::kFeatureDLSS};
  sl::PreferenceFlags flags =
      sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
#if defined(LARECOMP_HAS_DLSS_G)
  if (frame_gen) {
    std::string fg_why;
    if (FrameGenFilesOk(dir, &fg_why)) {
      features.push_back(sl::kFeatureDLSS_G);
      features.push_back(sl::kFeatureReflex);
      features.push_back(sl::kFeaturePCL);
      flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
      rt.frame_gen_asked = true;
      rt.frame_gen_loaded = true;
    } else {
      REXLOG_INFO("{} frame generation is left out of Streamline's start: {}", kPrefix, fg_why);
      std::lock_guard fg_lock(g_fg_mutex);
      g_fg_facts.why = fg_why;
    }
  }
#else
  (void)frame_gen;
#endif
  const std::wstring logs = StreamlineLogFolder();
  sl::Preferences pref;
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = plugin_paths;
  pref.numPathsToPlugins = 1;
  pref.pathToLogsAndData = logs.empty() ? nullptr : logs.c_str();
  pref.logMessageCallback = &SlLog;
  pref.flags = flags;
  pref.featuresToLoad = features.data();
  pref.numFeaturesToLoad = uint32_t(features.size());
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

#if defined(LARECOMP_HAS_DLSS_G)
// Streamline's answer on frame generation, once the device is set (slSetD3DDevice, or the Vulkan
// device made through the interposer): whether the feature is supported here, its functions and the
// functions of the Reflex and PC latency plugins it requires, and what slDLSSGGetState says (the most
// generated frames, the smallest size, vertical sync). Then the frame generation plugin is unloaded
// until it is wanted: a loaded plugin takes over the swap chains made after it, and Plume's chain
// is already made without it (dlss::LoadFrameGeneration). Never fails Super Resolution.
void QueryFrameGenerationLocked(Runtime& rt, const sl::AdapterInfo& adapter,
                                const std::string& gpu_name) {
  dlss::FrameGenerationFacts facts;
  {
    std::lock_guard fg_lock(g_fg_mutex);
    facts = g_fg_facts;  // `why` of a start that left the plugins out
  }
  facts.asked = rt.frame_gen_asked;
  facts.queried = true;
  if (!rt.frame_gen_asked) {
    if (facts.why.empty()) {
      facts.why = "Streamline was started without the frame generation plugins";
    }
    std::lock_guard fg_lock(g_fg_mutex);
    g_fg_facts = facts;
    return;
  }
  std::string why;
  const auto function_of = [&rt, &why](sl::Feature feature, const char* name) -> void* {
    void* function = nullptr;
    const sl::Result r = rt.api.get_function(feature, name, function);
    if (r != sl::Result::eOk || !function) {
      why = std::format("Streamline has no {}: {}", name, ResultText(r));
      return nullptr;
    }
    return function;
  };
  const sl::Result g = rt.api.is_supported(sl::kFeatureDLSS_G, adapter);
  const sl::Result reflex = g == sl::Result::eOk ? rt.api.is_supported(sl::kFeatureReflex, adapter)
                                                 : sl::Result::eOk;
  if (g != sl::Result::eOk) {
    why = "Streamline says DLSS frame generation cannot run on '" + gpu_name + "': " + ResultText(g);
  } else if (reflex != sl::Result::eOk) {
    why = "Streamline says Reflex, which frame generation requires, cannot run on '" + gpu_name +
          "': " + ResultText(reflex);
  } else {
    void* set_options = function_of(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    void* get_state = set_options ? function_of(sl::kFeatureDLSS_G, "slDLSSGetState") : nullptr;
    void* reflex_options = get_state ? function_of(sl::kFeatureReflex, "slReflexSetOptions") : nullptr;
    void* reflex_sleep = reflex_options ? function_of(sl::kFeatureReflex, "slReflexSleep") : nullptr;
    void* pcl_marker = reflex_sleep ? function_of(sl::kFeaturePCL, "slPCLSetMarker") : nullptr;
    if (pcl_marker) {
      rt.api.dlssg_set_options = reinterpret_cast<PFun_slDLSSGSetOptions*>(set_options);
      rt.api.dlssg_get_state = reinterpret_cast<PFun_slDLSSGGetState*>(get_state);
      rt.api.reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions*>(reflex_options);
      rt.api.reflex_sleep = reinterpret_cast<PFun_slReflexSleep*>(reflex_sleep);
      rt.api.pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker*>(pcl_marker);
      sl::DLSSGState state;
      const sl::Result r = rt.api.dlssg_get_state(sl::ViewportHandle(0u), state, nullptr);
      if (r == sl::Result::eOk) {
        facts.supported = true;
        facts.max_frames_generated = state.numFramesToGenerateMax;
        facts.min_size = state.minWidthOrHeight;
        facts.vsync_supported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        facts.dynamic_supported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
      } else {
        why = "slDLSSGGetState failed: " + ResultText(r);
      }
    }
  }
  facts.why = facts.supported ? std::string() : why;
  // Reflex and the markers work while frame generation is off (and are used only while it is on).
  g_reflex_ready.store(facts.supported, std::memory_order_release);
  if (rt.api.set_feature_loaded) {
    const sl::Result r = rt.api.set_feature_loaded(sl::kFeatureDLSS_G, false);
    rt.frame_gen_loaded = r != sl::Result::eOk;
    if (r != sl::Result::eOk) {
      REXLOG_WARN("{} the frame generation plugin could not be unloaded after the support query: {}; "
                  "it stays loaded, so Streamline's wrapper of the swap chain is the plugin's",
                  kPrefix, ResultText(r));
    }
  }
  {
    std::lock_guard fg_lock(g_fg_mutex);
    g_fg_facts = facts;
  }
  if (facts.supported) {
    REXLOG_INFO("{} DLSS frame generation is supported on '{}': up to {}x ({} generated frame(s) at "
                "most), the smallest size {}, vertical sync {}, dynamic multi frame {}",
                kPrefix, gpu_name, facts.max_frames_generated + 1, facts.max_frames_generated,
                facts.min_size, facts.vsync_supported ? "yes" : "no",
                facts.dynamic_supported ? "yes" : "no");
  } else {
    REXLOG_INFO("{} DLSS frame generation is not offered: {}", kPrefix, facts.why);
  }
}
#else
void QueryFrameGenerationLocked(Runtime&, const sl::AdapterInfo&, const std::string&) {
  std::lock_guard fg_lock(g_fg_mutex);
  g_fg_facts.queried = true;
  g_fg_facts.why = "this build has no DLSS frame generation";
}
#endif

sl::float4x4 ToSl(const float* m) {
  sl::float4x4 out;
  for (uint32_t r = 0; r < 4; ++r) {
    out.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
  }
  return out;
}

// Vulkan: the view must be a whole-image view usable for the image's usage flags.
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

// What every evaluation says about the game's projection and vectors, whoever records it (a frame,
// or the warm-up): DLSS reads depthInverted and the vector flags when it makes its feature, so the
// two must agree. Every field Streamline validates is given a value; cameraPinholeOffset (the
// camera has none) is zero, not left invalid.
void FillStaticConstants(sl::Constants& consts) {
  consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
  consts.depthInverted = sl::Boolean::eTrue;           // the game stores 1 near, 0 far
  consts.cameraMotionIncluded = sl::Boolean::eTrue;    // the vectors are the camera's motion
  consts.motionVectors3D = sl::Boolean::eFalse;
  consts.orthographicProjection = sl::Boolean::eFalse;
  consts.motionVectorsDilated = sl::Boolean::eFalse;
  consts.motionVectorsJittered = sl::Boolean::eFalse;  // the jitter is taken out of them
}

void SetIdentity(float (&m)[16]) {
  for (uint32_t i = 0; i < 16; ++i) {
    m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
}

// One context: a viewport of its own (its resources are freed after the frames that may use them,
// by which time the next context has started, so the ids are never reused) and the sizes and mode
// its options were set for.
struct Context {
  BackendRequest request;
  uint32_t viewport_id = 0;
  bool dispatched = false;    // render thread: the first dispatch resets the history
  bool created = false;       // the feature was made in the background (Warmup), not by the first dispatch
  bool bytes_asked = false;   // under Runtime::mu
  uint64_t bytes = 0;         // the video memory Streamline reports for it (0 until asked)
  uint64_t made_ms = 0;       // how long its feature took to make (pre-warm steps only: they log it)
};

// What the creation of the feature needs outside the frames' own objects: a command queue of its
// own (a second direct queue Plume makes: on D3D12 a queue of its own, on Vulkan a queue of the same
// family that Plume shares under the queue's mutex, so a submission never races a frame's), a list
// and a fence. Made once, on the render thread (Plume's create calls are the render thread's), used
// by the worker only, and destroyed after it has stopped.
struct SideKit {
  std::unique_ptr<plume::RenderCommandQueue> queue;  // first: destroyed last
  std::unique_ptr<plume::RenderCommandList> list;
  std::unique_ptr<plume::RenderCommandFence> fence;
  bool usable = false;
};

// The four images of the warm-up dispatch, the sizes of the request (the render size for the colour,
// the depth and the vectors, the output's for the output). Made on the render thread (Device::Create*),
// handed to the worker with the request, destroyed once its list has finished on the GPU.
struct WarmImages {
  std::unique_ptr<plume::RenderTexture> color;
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderTexture> motion;
  std::unique_ptr<plume::RenderTexture> output;
};

std::unique_ptr<SideKit> MakeSideKit(Device& device, std::string* why) {
  plume::RenderDevice* plume_device = device.device();
  if (!plume_device || device.IsLost()) {
    *why = "there is no graphics device";
    return {};
  }
  auto kit = std::make_unique<SideKit>();
  kit->queue = plume_device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!kit->queue || backend::IsNullBacked(kit->queue.get())) {
    *why = "Plume could not make a second queue";
    return {};
  }
  kit->list = kit->queue->createCommandList();
  kit->fence = plume_device->createCommandFence();
  if (!kit->list || !kit->fence || !backend::NativeListHandle(kit->list.get())) {
    *why = "Plume could not make a command list and a fence for it";
    return {};
  }
  kit->usable = true;
  return kit;
}

// The inputs of the warm-up dispatch: the images above, a still camera and no motion. The picture
// DLSS makes of them is never looked at; what counts is that every size, format and flag is what a
// frame's is, so the feature it makes is the one the frames use.
TemporalInputs MakeWarmupInputs(const BackendRequest& request, const WarmImages& images) {
  TemporalInputs in;
  in.render_size[0] = request.render_size[0];
  in.render_size[1] = request.render_size[1];
  in.output_size[0] = request.output_size[0];
  in.output_size[1] = request.output_size[1];
  in.color = {images.color.get(), in.render_size[0], in.render_size[1], kColorFormat, kColorFlags};
  in.depth = {images.depth.get(), in.render_size[0], in.render_size[1], kDepthFormat,
              kDepthMotionFlags};
  in.motion = {images.motion.get(), in.render_size[0], in.render_size[1], kMotionFormat,
               kDepthMotionFlags};
  in.output = {images.output.get(), in.output_size[0], in.output_size[1], kColorFormat,
               kOutputFlags};
  in.reset = true;
  in.camera_aspect = float(in.output_size[0]) / float(std::max(in.output_size[1], 1u));
  SetIdentity(in.view_to_clip);
  SetIdentity(in.clip_to_view);
  SetIdentity(in.clip_to_prev_clip);
  SetIdentity(in.prev_clip_to_clip);
  in.camera_up[1] = 1.0f;
  in.camera_right[0] = 1.0f;
  in.camera_forward[2] = 1.0f;
  return in;
}

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

  // ---- render thread only
  bool kit_tried = false;       // the side kit was asked for (made or not)
  uint64_t dispatches = 0;

  // ---- shared (under `mu`)
  mutable std::mutex mu;
  std::condition_variable cv;
  std::thread worker;
  bool stop = false;
  BackendStatus status;
  bool probed = false;          // the answer the menu reads is final
  bool needs_setup = false;     // an RTX card whose DLSS files are missing or unusable
  bool after_restart = false;   // Vulkan: it runs from a start with DLSS chosen
  bool init_done = false;       // the start-up (load, slInit, device, support) succeeded
  bool want = false;            // a context for `want_request` is wanted
  BackendRequest want_request;
  std::unique_ptr<WarmImages> want_images;  // for `want_request`, null when they could not be made
  std::shared_ptr<Context> ctx; // the ready context
  // The prepared context (make before break, Prepare/Commit): made in a slot of its own, by the same
  // worker one at a time, while `ctx` goes on dispatching, and taken over by `ctx` at Commit.
  bool next_want = false;
  BackendRequest next_request;
  std::unique_ptr<WarmImages> next_images;
  std::shared_ptr<Context> next_ctx;
  std::vector<std::shared_ptr<Context>> doomed;  // to be freed by the worker
  std::unique_ptr<SideKit> kit;
  std::atomic<bool>& building = g_building;  // the worker is making a context
  // Streamline's frame index: its own count, always rising, from any thread that evaluates.
  std::atomic<uint32_t> frame_counter{0};

  // ---- the pre-warm (under `mu`, except `prewarm_pending`): the first-time work of NGX and the driver
  // done in the background, with nothing kept (Prewarm). Armed by the probe when this PC can run DLSS
  // and it is not the start's choice; ended by a real request, a failure or its own last step.
  bool prewarm_armed = false;
  std::atomic<bool> prewarm_pending{false};  // armed, not finished and not dropped: read every frame
  std::vector<UpscaleMode> prewarm_modes;    // still to do, in order (the saved mode first)
  uint32_t prewarm_total = 0;                // the steps it was armed with
  uint32_t prewarm_done = 0;                 // finished by the worker (made and freed, or refused)
  double prewarm_slowest_ms = 0.0;
  bool prewarm_job = false;                  // a step is handed over and the worker has not begun it
  bool prewarm_busy = false;                 // ... or it has, and has not ended it: one step at a time
  BackendRequest prewarm_request;
  std::unique_ptr<WarmImages> prewarm_images;
  std::chrono::steady_clock::time_point prewarm_began{};  // the first step's start
  bool prewarm_start_logged = false;

  // ---- which modes DLSS takes at an output size (AskModes, Modes), under `mu`. A record is one mode at
  // one pair of sizes: made by the worker's query (RunFitJob) or by a context that was refused for
  // them (JobLoop). Newest last, bounded: a few output sizes are in use at a time.
  struct FitRecord {
    UpscaleMode mode = UpscaleMode::kNativeAa;
    uint32_t output[2] = {};
    uint32_t render[2] = {};
    bool fits = true;
    std::string why;
  };
  static constexpr size_t kMaxFitRecords = 64;
  std::vector<FitRecord> fits;
  ModeSizes fit_ask;            // the sizes the caller works at now
  bool fit_asked = false;
  bool fit_dirty = false;       // a mode of `fit_ask` has no record: the worker asks Streamline

  void PublishLocked() {
    PublishDlssAvailability(probed, status.dlss && !status.failed, needs_setup, after_restart);
  }
  // Under `mu`.
  const FitRecord* FindFitLocked(UpscaleMode mode, const uint32_t output[2],
                                 const uint32_t render[2]) const;
  void RecordFitLocked(UpscaleMode mode, const uint32_t output[2], const uint32_t render[2],
                       bool fits_now, const std::string& why);
  bool RefusedLocked(const BackendRequest& request) const;
  bool UnknownModesLocked(const ModeSizes& sizes) const;
  // The modes refused at the sizes asked for, to the pause menu (gpu.h dlss_modes_refused).
  void PublishModesLocked();
  bool FitJobLocked() const { return fit_dirty && init_done && !status.failed && !stop; }
  // On the worker, `lock` held on entry and on exit: asks Streamline about every mode of `fit_ask`
  // that has no record. Only the options and a size query: no feature, no video memory.
  void RunFitJob(std::unique_lock<std::mutex>& lock);
  void StartWorkerLocked();
  void WorkerMain();
  void JobLoop();
  // Under `mu`: a context is wanted and not there yet, for the current slot and the prepared one.
  bool WantCurrentLocked() const { return want && !ctx && !status.failed; }
  bool WantNextLocked() const { return next_want && !next_ctx && !status.failed; }
  // Under `mu`: the prepared request is not wanted any more (a context being made for it is freed
  // by the worker when it is done).
  void DropNextLocked(std::vector<std::shared_ptr<Context>>* retire) {
    next_want = false;
    next_images.reset();
    if (next_ctx) {
      retire->push_back(std::move(next_ctx));
      next_ctx.reset();
    }
  }
  // Under `mu`: the worker has a pre-warm step to make. A real request (the current or the prepared
  // slot) always comes first, and drops the pre-warm besides (DropPrewarmLocked).
  bool PrewarmJobLocked() const { return prewarm_job && !status.failed && !stop; }
  // Under `mu`: arms the pre-warm for the modes in the order of the player's likely use: the saved
  // DLSS mode first, then the others. Probe only.
  void ArmPrewarmLocked();
  // Under `mu`: the pre-warm goes (a real request, a failure, the end of the game). True when one was
  // pending, so that the caller says so once, outside the lock.
  bool DropPrewarmLocked();
  // The reason a pre-warm step for `request` should not take video memory now (the card is nearly
  // full), empty when it may. Render thread.
  std::string PrewarmMemoryProblem(const BackendRequest& request) const;
  // Render thread: the side queue, list and fence, once (made or not).
  void EnsureKit();
  bool StartRuntime(std::string* why, bool* files_problem);
  void PublishEarlyVerdict();
  void FailStart(const std::string& why, bool files_problem);
  void FailLocked(std::unique_lock<std::mutex>& lock, const std::string& why, bool render_thread);
  std::unique_ptr<WarmImages> MakeImages(const BackendRequest& request);
  // Null with `why` when no context was made. `*refused` says whether that is a size refusal (DLSS
  // does not take this mode at these sizes: not a failure, the backend stays available) or a real
  // failure.
  std::shared_ptr<Context> MakeContext(const BackendRequest& request, std::string* why, bool prewarm,
                                       bool* refused);
  // `prewarm`: a pre-warm step (Prewarm): the context is made to be freed, and a feature that
  // could not be made is `created == false` with `why` set, never "the first frame makes it".
  std::shared_ptr<Context> Build(const BackendRequest& request, WarmImages* images, SideKit* side,
                                 std::string* why, bool prewarm, bool* refused);
  bool Warmup(Context& context, WarmImages& images, SideKit& side, std::string* error);
  // `wait` false (a frame's dispatch on the render thread): Streamline's mutex is tried first, and
  // when the worker holds it to make a feature (`building`) `*busy` is set and nothing is recorded.
  std::string Evaluate(void* native_list, Context& context, const TemporalInputs& in, bool wait,
                       bool* busy);
  void FreeContext(const Context& context);
  // Render thread: the contexts go to the worker once the frames that may use them have finished.
  void RetireOutsideLock(std::vector<std::shared_ptr<Context>>& retire);
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
      // The Vulkan device is made with Streamline's help only when DLSS is the saved choice at the
      // start (BeforeDevice: the start takes seconds longer for it), so a game started with
      // anything else cannot switch to DLSS while it runs: an RTX card lists it for the next start
      // ("after restart": the pause menu's UPSCALING row says RESTART and offers it), and nothing is
      // warmed up (there is no live switch to be quick). The same when the GPU Plume took is not the
      // one BeforeDevice looked at, or a build without Plume's hooks.
#if defined(LARECOMP_PLUME_STREAMLINE)
      if (rtx_name) {
        REXLOG_INFO("{} listed for Vulkan, but this game's device was made without Streamline (it "
                    "is made through it only when DLSS is the saved choice at the start: that "
                    "costs seconds at every start): choosing DLSS applies from the next start",
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
    // Started before the device, which only happens when DLSS is the saved choice: the worker
    // finishes it (the support query). The files are known good, so an RTX card is published as
    // able to run it as soon as the worker has looked at them (PublishEarlyVerdict); the query is
    // not waited for by the start decision. No pre-warm: the context made at the start pays for
    // its kind of feature.
    std::lock_guard lock(i.mu);
    i.status.state = BackendState::kProbing;
    i.StartWorkerLocked();
    return;
  }

  // D3D12: Streamline starts now when DLSS is the saved choice (the start's scene size needs the
  // answer), otherwise in the background a few seconds after the game first draws (the pre-warm,
  // PrewarmNext) or at its first use, whichever comes first. Its start (slInit and NGX) takes from
  // 2 s to over 10 s on a cold start, so the answer the start decision waits for is not that one:
  // the worker publishes "can run" (PublishEarlyVerdict) once the files, their signatures and the
  // card's name are checked, and finishes the start in the background.
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
  REXLOG_INFO("{} listed: '{}' is an RTX card and the files are there; Streamline starts in the "
              "background a few seconds after the game first draws (the warm-up), or at once "
              "when DLSS is chosen before that",
              kPrefix, i.gpu_name);
  {
    // D3D12 starts Streamline on the worker (slInit and NGX: about 10 s), so the pre-warm's first
    // step is its start (PrewarmNext). Armed before the answer is published.
    std::lock_guard lock(i.mu);
    i.ArmPrewarmLocked();
  }
  settle(BackendState::kAvailable, {}, true, false, false);
}

// The fast part of the answer, from the worker, once the files are there, signed and opened (the
// worker's first step, a fraction of a second): an RTX card with usable files can run DLSS. That is
// what the start decision (temporal_backend.cpp DecideStartUpscale) waits for, so a slow Streamline
// start (slInit, NGX and the support query: seconds) never decides the scene's size. If the rest
// of the start fails, FailStart says so plainly and the game goes on without DLSS. A card whose
// name does not say RTX waits for the support query (the final word) as before.
void DlssBackend::Impl::PublishEarlyVerdict() {
  if (!NameLooksLikeRtx(gpu_name)) {
    return;
  }
  {
    std::lock_guard lock(mu);
    if (probed) {
      return;
    }
    status.dlss = true;
    probed = true;
    needs_setup = false;
    after_restart = false;
    PublishLocked();
  }
  REXLOG_INFO("{} '{}' is an RTX card and its DLSS files are in place and signed: DLSS counts as "
              "able to run; Streamline finishes starting in the background",
              kPrefix, gpu_name);
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
  JobLoop();
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
  PublishEarlyVerdict();
  if (!rt.initialized) {
    if (api == Backend::kVulkan) {
      *why = "on Vulkan Streamline has to start before the device is made";
      return false;
    }
    if (!InitLocked(rt, sl::RenderAPI::eD3D12, WantFrameGenerationPlugins(api, gpu_name), why)) {
      rt.failure = *why;
      return false;
    }
  }
  if (api == Backend::kD3D12) {
    // The manual hooking guide, "slSetD3DDevice": "When using D3D simply create your device and
    // call slSetD3DDevice". Super Resolution needs no proxy of the device or the factory. The
    // swap chain is the one thing that does get a wrapper (slUpgradeInterface): the common
    // plugin's presentCommon runs from its Present, and without it Streamline logs
    // "presentCommon() was not observed" and, as its guide says, grows its memory across swap
    // chain resizes. The wrapper goes in once this start has ended (pending flag, below), not at
    // the swap chain's creation: see UpgradeSwapChain.
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
  QueryFrameGenerationLocked(rt, adapter, gpu_name);
  if (api == Backend::kD3D12 && rt.api.upgrade_interface) {
    // The plugins have started (slSetD3DDevice ran their start and filled the hook lists the
    // swap chain's wrapper reads): the next present may hand Streamline the swap chain.
    g_swap_chain_pending.store(true, std::memory_order_release);
  }
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
    DropPrewarmLocked();  // nothing to warm: the start failed (said below)
    PublishLocked();
    PublishModesLocked();
  }
  // What the player sees now, when the start decided on DLSS before this failed (the decision does
  // not wait for Streamline's slow start): the game goes on without it.
  std::string effect;
  const StartUpscale start = PeekStartUpscale();
  if (start.decided && start.runs && start.upscaler == Upscaler::kDlss) {
    effect = start.ratio > 1.0
                 ? std::format(". This start drew the 3D scene at {}% of the output for DLSS, and "
                               "nothing upscales it now: the scene is moved back to full size at "
                               "the next frame boundary (a live change), soft until then",
                               ModeInfoOf(Upscaler::kDlss, start.mode).percent)
                 : std::string(". The picture is the game's own at full size, with none of DLSS's "
                               "smoothing");
  }
  REXLOG_WARN("{} DLSS is off for this session: {}{}{}", kPrefix, why,
              listed_setup ? ". Run the game's setup again to get its files" : "", effect);
}

// ---------------------------------------------------------------------------
// the render thread's calls
// ---------------------------------------------------------------------------

BackendStatus DlssBackend::Status() const {
  std::lock_guard lock(impl_->mu);
  return impl_->status;
}

void DlssBackend::Impl::FailLocked(std::unique_lock<std::mutex>& lock, const std::string& why,
                                   bool render_thread) {
  // Under `mu`. One log line; DLSS is off for the session, the context goes after the frames that
  // may use it.
  const bool first = !status.failed;
  status.failed = true;
  status.state = BackendState::kUnavailable;
  status.dlss = false;
  status.reason = why;
  status.context_bytes = 0;
  want = false;
  want_images.reset();
  DropPrewarmLocked();
  std::vector<std::shared_ptr<Context>> list;
  if (ctx) {
    list.push_back(std::move(ctx));
    ctx.reset();
  }
  DropNextLocked(&list);
  PublishLocked();
  PublishModesLocked();  // nothing is refused for a DLSS that is off
  lock.unlock();
  if (first) {
    REXLOG_WARN("{} DLSS is off for this session: {}", kPrefix, why);
  }
  if (!list.empty()) {
    if (render_thread) {
      RetireOutsideLock(list);
    } else {
      {
        std::lock_guard guard(mu);
        for (std::shared_ptr<Context>& old : list) {
          doomed.push_back(std::move(old));
        }
      }
      cv.notify_all();
    }
  }
  lock.lock();
}

void DlssBackend::Impl::FreeContext(const Context& context) {
  // On the worker, after the frames that may use it, so no dispatch of this viewport is pending on
  // a list.
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (!rt.initialized || rt.shut_down || !rt.api.free_resources) {
    return;
  }
  rt.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(context.viewport_id));
}

void DlssBackend::Impl::RetireOutsideLock(std::vector<std::shared_ptr<Context>>& retire) {
  // The context may be named by a list the GPU has not finished: it goes to the worker (which frees
  // it, and may be busy for seconds with another context) once the frames recorded so far are done.
  for (std::shared_ptr<Context>& old : retire) {
    if (!frames) {
      {
        std::lock_guard lock(mu);
        doomed.push_back(std::move(old));
      }
      cv.notify_all();
      continue;
    }
    frames->RetireAction([this, old] {
      {
        std::lock_guard lock(mu);
        doomed.push_back(old);
      }
      cv.notify_all();
    });
  }
  retire.clear();
}

void DlssBackend::Impl::EnsureKit() {
  if (kit_tried) {
    return;
  }
  kit_tried = true;
  std::string why;
  std::unique_ptr<SideKit> made = MakeSideKit(*device, &why);
  if (!made) {
    REXLOG_WARN("{} no background creation of the feature: {}", kPrefix, why);
  }
  std::lock_guard lock(mu);
  kit = std::move(made);
}

// ---------------------------------------------------------------------------
// the pre-warm
// ---------------------------------------------------------------------------
// What was measured (RTX 5080, live switching runs 2026-11-06, D3D12): the first feature of a
// process took 1.2 s (Performance at 640x360) and the first DLAA one 8.4 s (1280x720 to 1280x720),
// the Vulkan runs' first feature (Quality at 832x468) 8.4 to 8.7 s, and every later one 25 to 30 ms,
// Quality, Performance and DLAA alike, including after all earlier features had been freed by the
// live switches. So the slow part is first-time work of NGX and the driver per kind of feature, not a
// property of one live context; paying it once in the background, with a feature that is freed at
// once (no video memory is kept), leaves every later creation at its 30 ms. Which sizes and modes
// make a kind is not known (the runs cannot tell a mode's network from the size's class), so one
// step makes each of the five modes at the real sizes, the saved mode first. Only where DLSS can be
// switched on while the game runs, i.e. D3D12 (a Vulkan game has it only when it was the start's
// choice, whose context at the start paid for its kind).

void DlssBackend::Impl::ArmPrewarmLocked() {
  if (prewarm_armed) {
    return;
  }
  prewarm_armed = true;
  const UpscaleMode saved = SavedUpscaleMode(Upscaler::kDlss);
  prewarm_modes.clear();
  prewarm_modes.push_back(saved);
  for (const UpscaleMode mode : {UpscaleMode::kNativeAa, UpscaleMode::kQuality,
                                 UpscaleMode::kPerformance, UpscaleMode::kBalanced,
                                 UpscaleMode::kUltraPerformance}) {
    if (mode != saved) {
      prewarm_modes.push_back(mode);
    }
  }
  prewarm_total = uint32_t(prewarm_modes.size());
  prewarm_pending.store(true, std::memory_order_release);
}

bool DlssBackend::Impl::DropPrewarmLocked() {
  const bool was_pending = prewarm_pending.exchange(false, std::memory_order_acq_rel);
  prewarm_modes.clear();
  if (prewarm_job) {
    // Handed over and never begun: nobody else ends it (a begun step is ended by the worker).
    prewarm_busy = false;
  }
  prewarm_job = false;
  prewarm_images.reset();
  return was_pending;
}

std::string DlssBackend::Impl::PrewarmMemoryProblem(const BackendRequest& request) const {
  if (!device) {
    return "there is no graphics device";
  }
  const backend::VideoMemory vram = device->QueryVideoMemory();
  if (vram.budget == 0) {
    return {};  // the backend reports none: nothing to compare with (the vram budget's own rule)
  }
  const uint32_t rw = request.render_size[0];
  const uint32_t rh = request.render_size[1];
  const uint32_t ow = request.output_size[0];
  const uint32_t oh = request.output_size[1];
  const uint64_t images = TextureBytes(rw, rh, 1, kColorFormat) + TextureBytes(rw, rh, 1, kDepthFormat) +
                          TextureBytes(rw, rh, 1, kMotionFormat) + TextureBytes(ow, oh, 1, kColorFormat);
  const uint64_t need = images + kPrewarmFeatureReserveBytes;
  if (vram.used + need > vram.budget / 100 * VideoMemoryBudget::kHighPercent) {
    return std::format("video memory is nearly full ({} of {} MiB in use, a step needs about {} MiB)",
                       vram.used >> 20, vram.budget >> 20, need >> 20);
  }
  return {};
}

// The four images of the warm-up for this request, on the render thread. Null when there is no side
// kit or an image cannot be made (the feature is then made by the first dispatch, as without the
// warm-up).
std::unique_ptr<WarmImages> DlssBackend::Impl::MakeImages(const BackendRequest& request) {
  if (!device || !kit || !kit->usable || device->IsLost()) {
    return nullptr;
  }
  auto images = std::make_unique<WarmImages>();
  const uint32_t rw = request.render_size[0];
  const uint32_t rh = request.render_size[1];
  const uint32_t ow = request.output_size[0];
  const uint32_t oh = request.output_size[1];
  images->color = device->CreateTexture(
      plume::RenderTextureDesc::Texture2D(rw, rh, 1, kColorFormat, kColorFlags), "dlss warm-up color");
  images->depth = device->CreateTexture(
      plume::RenderTextureDesc::Texture2D(rw, rh, 1, kDepthFormat, kDepthMotionFlags),
      "dlss warm-up depth");
  images->motion = device->CreateTexture(
      plume::RenderTextureDesc::Texture2D(rw, rh, 1, kMotionFormat, kDepthMotionFlags),
      "dlss warm-up vectors");
  images->output = device->CreateTexture(
      plume::RenderTextureDesc::Texture2D(ow, oh, 1, kColorFormat, kOutputFlags),
      "dlss warm-up output");
  if (!images->color || !images->depth || !images->motion || !images->output) {
    return nullptr;
  }
  return images;
}

// The fast part of a context, on the worker: the options of its viewport (mode, output size) and the
// check that the scene's size is one DLSS takes. The feature itself is made by Build's warm-up, or
// failing that by the first dispatch.
std::shared_ptr<Context> DlssBackend::Impl::MakeContext(const BackendRequest& request,
                                                        std::string* why, bool prewarm,
                                                        bool* refused) {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  *refused = false;
  if (!rt.initialized || rt.shut_down || !rt.api.dlss_optimal || !rt.api.dlss_set_options) {
    *why = "Streamline is not running";
    return {};
  }
  const sl::DLSSOptions options =
      OptionsFor(request.mode, request.output_size[0], request.output_size[1]);

  // The size DLSS wants for this mode and output. The scene was drawn at the start's size (the
  // vendor's ratio over the render scale: temporal_backend.cpp kDlssModes), which can differ from
  // it by a few pixels, or by a mode picked since from another vendor's list: anything in DLSS's
  // dynamic range works (SceneFits). A size it does not take is a refusal, not a failure: the
  // caller records it (JobLoop) and goes on with the mode that runs.
  sl::DLSSOptimalSettings optimal;
  const sl::Result got = rt.api.dlss_optimal(options, optimal);
  if (got != sl::Result::eOk) {
    *why = "slDLSSGetOptimalSettings failed: " + ResultText(got);
    return {};
  }
  const uint32_t rw = request.render_size[0];
  const uint32_t rh = request.render_size[1];
  if (!SceneFits(optimal, rw, rh)) {
    *why = SceneWhy(optimal, rw, rh, request.output_size[0], request.output_size[1]);
    *refused = true;
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
  REXLOG_INFO("{} {} for {} at {}x{} to {}x{} (viewport {}): DLSS asks for {}x{} (range {}x{} to "
              "{}x{})",
              kPrefix, prewarm ? "warm-up context" : "context",
              ModeInfoOf(Upscaler::kDlss, request.mode).name, rw, rh,
              request.output_size[0], request.output_size[1], context->viewport_id,
              optimal.optimalRenderWidth, optimal.optimalRenderHeight, optimal.renderWidthMin,
              optimal.renderHeightMin, optimal.renderWidthMax, optimal.renderHeightMax);
  return context;
}

// A context for `request`, on the worker: the options (MakeContext) and then the feature itself.
// DLSS builds its network inside the first slEvaluateFeature of a viewport (NGX CreateFeature: 8.2 s
// on an RTX 5080, on whichever thread it is called), so that first evaluation is made here, on the
// worker, with a command list, queue and fence of its own and images of the right sizes
// (Warmup), while the render thread goes on with plain frames. The viewport then has its feature
// and the first evaluation of a frame is as fast as the later ones. If the warm-up cannot be made
// (no side kit or images, or it fails) the context is returned anyway and the first frame makes the
// feature, with the pause the warm-up was there to avoid.
std::shared_ptr<Context> DlssBackend::Impl::Build(const BackendRequest& request,
                                                  WarmImages* images, SideKit* side,
                                                  std::string* why, bool prewarm, bool* refused) {
  std::shared_ptr<Context> context = MakeContext(request, why, prewarm, refused);
  if (!context) {
    return {};
  }
  if (!images || !side || !side->usable) {
    if (prewarm) {
      *why = "the objects for it could not be made";  // created stays false
    } else {
      REXLOG_WARN("{} the feature is not made in the background (the objects for it could not be "
                  "made): the first frame makes it, which pauses the game for several seconds",
                  kPrefix);
    }
    return context;
  }
  const auto started = std::chrono::steady_clock::now();
  std::string error;
  if (!Warmup(*context, *images, *side, &error)) {
    if (prewarm) {
      *why = error;  // created stays false
    } else {
      REXLOG_WARN("{} the feature could not be made in the background ({}): the first frame makes "
                  "it instead, which pauses the game for several seconds",
                  kPrefix, error);
    }
    return context;
  }
  context->created = true;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  if (prewarm) {
    context->made_ms = uint64_t(ms);  // the worker's step line says it (JobLoop)
    return context;
  }
  REXLOG_INFO("{} the feature for {} at {}x{} to {}x{} (viewport {}) was made in the background in "
              "{} ms (about {} MB): the game went on meanwhile",
              kPrefix, ModeInfoOf(Upscaler::kDlss, request.mode).name, request.render_size[0],
              request.render_size[1], request.output_size[0], request.output_size[1],
              context->viewport_id, ms, context->bytes >> 20);
  return context;
}

// One evaluation of the context's viewport on the side queue, on the worker: the first one makes the
// feature. Outside the frames' lists and queue and on images no frame uses, so nothing it does can
// touch a frame.
// The feature is made on a side queue by a worker and used from the main queue after the fence wait.
// The dispatch over images nobody filled only warms the feature up; its output is never used.
//
// Where the creation's commands are: Streamline's sl.dlss makes the NGX feature inside the first
// slEvaluateFeature of a viewport (dlssBeginEvent calls NVSDK_NGX_VULKAN_CreateFeature1 /
// D3D12_CreateFeature on the command buffer or list given to slEvaluateFeature), and
// slAllocateResources runs the same function on the buffer it is given and needs the viewport's
// tags set first, so there is no separate creation list. Everything NGX records for the creation is
// in `list`, ahead of its evaluation, and is submitted and waited for here before the context is
// published: no frame can evaluate the feature earlier.
//
// Vulkan validation, run vv9 (2026-11-07, the switching cycle after the warm-up): 6 messages
// VUID-vkCmdDraw-None-09600 at this submit, "[nv.ngx.dlss.resource] expected GENERAL, current
// UNDEFINED", two NGX-owned images each time, all for the PERFORMANCE mode's feature (warm-up step
// 3 and both live switches to it), none for DLAA, Quality or Balanced, which this same path made
// without a message (vv5 as well). sl.log shows what differs: an NVIDIA override on this PC
// (SL_DLSS_OVERRIDE DRS key 1, "Using DRS Overridden Preset") gave preset K to DLAA, Quality and
// Balanced and preset M ("Override Reported: ModelPreset, Value: 13") to Performance; M's creation
// logs no PaddedWindowNetwork containers and its feature is about 13 MB bigger. M's evaluation uses
// two of its own images in GENERAL with no transition recorded for them in the buffer that made
// them: NGX's bookkeeping, not an ordering fault of ours (same buffer, submitted and fenced first,
// and the frames that evaluate it afterwards raise nothing).
// CHECK: that reading. Run the vv9 cycle again with NVIDIA's DLSS model override off (sl.log then
// names another preset for Performance and the messages should be gone), or without the PERFORMANCE
// steps (nothing expected). A message for a feature of preset K means it is something else.
bool DlssBackend::Impl::Warmup(Context& context, WarmImages& images, SideKit& side,
                               std::string* error) {
  const TemporalInputs in = MakeWarmupInputs(context.request, images);
  plume::RenderCommandList* list = side.list.get();
  list->begin();
  void* native_list = backend::NativeListHandle(list);
  if (!native_list) {
    list->end();
    *error = "the side command list has no native object";
    return false;
  }
  // The same states a frame's dispatch has the images in (temporal.cpp RecordUpscale).
  namespace Stage = plume::RenderBarrierStage;
  plume::RenderTextureBarrier to_read[3] = {
      plume::RenderTextureBarrier(images.color.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(images.depth.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(images.motion.get(), plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::COMPUTE, to_read, 3);
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(images.output.get(),
                                                              plume::RenderTextureLayout::GENERAL));
  bool busy = false;
  *error = Evaluate(native_list, context, in, /*wait=*/true, &busy);
  backend::InvalidateAfterExternalWork(list);
  list->end();
  if (!error->empty()) {
    return false;
  }
  side.queue->executeCommandLists(list, side.fence.get());
  side.queue->waitForCommandFence(side.fence.get());
  return true;
}

// ---------------------------------------------------------------------------
// which modes DLSS takes at an output size
// ---------------------------------------------------------------------------
// DLSS gives each mode one render size for an output size, with a range around it for every mode but
// Ultra Performance (SceneFits), and the scene is drawn at a size of the scale grid, 0.5x of the
// guest's 1280x720 at the least (render_scale.h ChooseSceneScale), so some modes cannot run at some
// outputs: Ultra Performance needs a third of the output, which is under 0.5x for an output under
// 1.5x (1280x720: 427x240 against 640x360) and between two steps of the grid at 2560x1440 (853x480
// against 832x468); it fits at 1920x1080 (640x360) and at 3840x2160 (1280x720). Such a mode is not a
// failure of DLSS: it is refused, the caller goes on with a mode that runs (temporal.cpp
// ResolveDlssMode), and the pause menu says "(N/A)" for it.
// CHECK: the ranges slDLSSGetOptimalSettings gives at 1280x720, 1920x1080, 2560x1440 and 3840x2160 for
// all five modes (the census line "at WxH these modes cannot run" lists what it refused; measured
// before: Ultra Performance exact, the others a range), and which modes this refuses at each.

auto DlssBackend::Impl::FindFitLocked(UpscaleMode mode, const uint32_t output[2],
                                      const uint32_t render[2]) const -> const FitRecord* {
  for (auto it = fits.rbegin(); it != fits.rend(); ++it) {
    if (it->mode == mode && it->output[0] == output[0] && it->output[1] == output[1] &&
        it->render[0] == render[0] && it->render[1] == render[1]) {
      return &*it;
    }
  }
  return nullptr;
}

void DlssBackend::Impl::RecordFitLocked(UpscaleMode mode, const uint32_t output[2],
                                        const uint32_t render[2], bool fits_now,
                                        const std::string& why) {
  bool found = false;
  for (FitRecord& r : fits) {
    if (r.mode == mode && r.output[0] == output[0] && r.output[1] == output[1] &&
        r.render[0] == render[0] && r.render[1] == render[1]) {
      r.fits = fits_now;
      r.why = why;
      found = true;
      break;
    }
  }
  if (!found) {
    FitRecord r;
    r.mode = mode;
    r.output[0] = output[0];
    r.output[1] = output[1];
    r.render[0] = render[0];
    r.render[1] = render[1];
    r.fits = fits_now;
    r.why = why;
    fits.push_back(std::move(r));
    if (fits.size() > kMaxFitRecords) {
      fits.erase(fits.begin());
    }
  }
  PublishModesLocked();
}

bool DlssBackend::Impl::RefusedLocked(const BackendRequest& request) const {
  const FitRecord* record = FindFitLocked(request.mode, request.output_size, request.render_size);
  return record && !record->fits;
}

bool DlssBackend::Impl::UnknownModesLocked(const ModeSizes& sizes) const {
  for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
    if (!FindFitLocked(UpscaleMode(mode), sizes.output, sizes.render[mode])) {
      return true;
    }
  }
  return false;
}

void DlssBackend::Impl::PublishModesLocked() {
  uint32_t refused = 0;
  if (fit_asked && !status.failed) {
    for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
      const FitRecord* record = FindFitLocked(UpscaleMode(mode), fit_ask.output, fit_ask.render[mode]);
      if (record && !record->fits) {
        refused |= 1u << mode;
      }
    }
  }
  PublishDlssRefusedModes(refused);
}

// CHECK: slDLSSGetOptimalSettings called five times in a row on the worker with no viewport and while
// the render thread dispatches (Runtime::mu serialises them; MakeContext makes the same call), and that
// a card or driver whose answer differs from the measured ranges still gets a sensible "(N/A)" set.
void DlssBackend::Impl::RunFitJob(std::unique_lock<std::mutex>& lock) {
  const ModeSizes asked = fit_ask;
  fit_dirty = false;
  bool wanted[size_t(UpscaleMode::kCount)] = {};
  bool any = false;
  for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
    wanted[mode] = !FindFitLocked(UpscaleMode(mode), asked.output, asked.render[mode]);
    any = any || wanted[mode];
  }
  if (!any) {
    return;
  }
  lock.unlock();
  struct Answer {
    UpscaleMode mode;
    bool fits;
    std::string why;
  };
  std::vector<Answer> answers;
  {
    Runtime& rt = Rt();
    std::lock_guard sl_lock(rt.mu);
    if (rt.initialized && !rt.shut_down && rt.api.dlss_optimal) {
      for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
        if (!wanted[mode]) {
          continue;
        }
        sl::DLSSOptimalSettings optimal;
        const sl::Result got = rt.api.dlss_optimal(
            OptionsFor(UpscaleMode(mode), asked.output[0], asked.output[1]), optimal);
        if (got != sl::Result::eOk) {
          continue;  // stays unknown: the context made for the mode decides (MakeContext)
        }
        const uint32_t rw = asked.render[mode][0];
        const uint32_t rh = asked.render[mode][1];
        const bool fits_now = SceneFits(optimal, rw, rh);
        answers.push_back({UpscaleMode(mode), fits_now,
                           fits_now ? std::string()
                                    : SceneWhy(optimal, rw, rh, asked.output[0], asked.output[1])});
      }
    }
  }
  lock.lock();
  std::string refused_names;
  for (const Answer& a : answers) {
    RecordFitLocked(a.mode, asked.output, asked.render[size_t(a.mode)], a.fits, a.why);
    if (!a.fits) {
      refused_names += (refused_names.empty() ? "" : ", ");
      refused_names += ModeInfoOf(Upscaler::kDlss, a.mode).name;
    }
  }
  if (!answers.empty()) {
    // Information, once per output size: what the pause menu will mark "(N/A)". What the renderer
    // does about it, if DLSS is in use, is said by the temporal system (ResolveDlssMode).
    const std::string text =
        refused_names.empty() ? std::string("all five modes can run")
                              : std::format("these cannot run (the scene is drawn on the scale grid, "
                                            "0.5x at the least): {}",
                                            refused_names);
    lock.unlock();
    REXLOG_INFO("{} at an output of {}x{}: {}", kPrefix, asked.output[0], asked.output[1], text);
    lock.lock();
  }
}

// The worker after the start-up: makes the context that is wanted, frees the contexts that are
// done with. Streamline calls that take seconds (the creation of a feature) happen here and
// nowhere else, with no lock but Runtime::mu held, which only this thread and a frame's own
// dispatch of a ready context ever take.
void DlssBackend::Impl::JobLoop() {
  std::unique_lock lock(mu);
  for (;;) {
    cv.wait(lock, [this] {
      return stop || FitJobLocked() || !doomed.empty() || WantCurrentLocked() || WantNextLocked() ||
             PrewarmJobLocked();
    });
    if (stop) {
      break;
    }
    // The sizes the caller works at come first: a size query, fast.
    if (FitJobLocked()) {
      RunFitJob(lock);
      continue;
    }
    if (!doomed.empty()) {
      std::vector<std::shared_ptr<Context>> list = std::move(doomed);
      doomed.clear();
      lock.unlock();
      for (const std::shared_ptr<Context>& old : list) {
        FreeContext(*old);
      }
      list.clear();
      lock.lock();
      continue;
    }
    // The current slot first (the frames wait for it), then the prepared one.
    const bool for_current = WantCurrentLocked();
    if (for_current || WantNextLocked()) {
      const BackendRequest request = for_current ? want_request : next_request;
      std::unique_ptr<WarmImages> images =
          std::move(for_current ? want_images : next_images);
      SideKit* side = kit.get();
      building = true;
      lock.unlock();
      std::string why;
      bool refused = false;
      std::shared_ptr<Context> made =
          Build(request, images.get(), side, &why, /*prewarm=*/false, &refused);
      images.reset();  // Build waited for the side queue: the GPU is done with them
      lock.lock();
      building = false;
      if (!made && refused) {
        // DLSS does not take this mode at these sizes: not a failure. It is remembered (a request
        // for it is not made again, and the pause menu says "(N/A)"), nothing is wanted any more
        // for it, and the caller goes on with what runs; it says so (temporal.cpp ResolveDlssMode).
        RecordFitLocked(request.mode, request.output_size, request.render_size, false, why);
        if (for_current) {
          if (want && want_request == request) {
            want = false;
            want_images.reset();
          }
        } else if (next_want && next_request == request) {
          next_want = false;
          next_images.reset();
        }
      } else if (!made) {
        FailLocked(lock, "its context could not be made: " + why, /*render_thread=*/false);
      } else if (for_current && want && want_request == request && !ctx && !status.failed &&
                 !stop) {
        status.context_bytes = made->bytes;
        ctx = std::move(made);
      } else if (!for_current && next_want && next_request == request && !next_ctx &&
                 !status.failed && !stop) {
        next_ctx = std::move(made);
      } else {
        // The request moved on (or DLSS failed, or the game is closing) while it was made: it goes.
        doomed.push_back(std::move(made));
      }
    } else if (PrewarmJobLocked()) {
      // A pre-warm step: one feature at the real sizes for one mode, made (which is what NGX and the
      // driver need seconds for the first time) and freed at once. A real request never waits for
      // more than the step in hand: it comes first here, and it ends the pre-warm (Request, Prepare).
      // Nothing of it reaches a frame: the context is not published, and a failure is not DLSS's
      // (the real request meets it for itself, and says so then).
      const BackendRequest request = prewarm_request;
      std::unique_ptr<WarmImages> images = std::move(prewarm_images);
      prewarm_job = false;
      SideKit* side = kit.get();
      building = true;
      lock.unlock();
      std::string why;
      bool refused = false;
      std::shared_ptr<Context> made =
          Build(request, images.get(), side, &why, /*prewarm=*/true, &refused);
      images.reset();  // Build waited for the side queue: the GPU is done with them
      const bool created = made && made->created;
      const uint64_t made_ms = made ? made->made_ms : 0;
      const uint64_t made_mb = made ? made->bytes >> 20 : 0;
      lock.lock();
      building = false;
      prewarm_busy = false;
      if (!made && refused) {
        RecordFitLocked(request.mode, request.output_size, request.render_size, false, why);
      }
      const uint32_t step = ++prewarm_done;
      const uint32_t total = prewarm_total;
      if (created) {
        prewarm_slowest_ms = std::max(prewarm_slowest_ms, double(made_ms));
      }
      const bool over = prewarm_pending.load() && prewarm_modes.empty();
      if (over) {
        prewarm_pending = false;
      }
      const double slowest = prewarm_slowest_ms;
      const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                           prewarm_began).count();
      if (made) {
        doomed.push_back(std::move(made));  // freed at the top of this loop, by this thread
      }
      lock.unlock();
      const char* mode_name = ModeInfoOf(Upscaler::kDlss, request.mode).name;
      if (created) {
        REXLOG_INFO("{} warm-up {}/{}: {} at {}x{} to {}x{}: the feature was made in {} ms (about {} "
                    "MB) and is freed again, nothing is kept",
                    kPrefix, step, total, mode_name, request.render_size[0], request.render_size[1],
                    request.output_size[0], request.output_size[1], made_ms, made_mb);
      } else if (!made) {
        REXLOG_INFO("{} warm-up {}/{}: {} at {}x{} to {}x{} skipped: {}", kPrefix, step, total,
                    mode_name, request.render_size[0], request.render_size[1],
                    request.output_size[0], request.output_size[1], why);
      } else {
        REXLOG_WARN("{} warm-up {}/{}: {} at {}x{} to {}x{}: the feature could not be made ({}); the "
                    "warm-up goes on, and a switch to this mode makes the feature itself",
                    kPrefix, step, total, mode_name, request.render_size[0], request.render_size[1],
                    request.output_size[0], request.output_size[1], why);
      }
      if (over) {
        REXLOG_INFO("{} warm-up done in {:.1f} s ({} steps, the slowest {:.0f} ms): the first-time work "
                    "of NGX and the driver is paid for, so DLSS chosen from the menu makes its "
                    "context in about the time of the later features (tens of milliseconds)",
                    kPrefix, total_s, total, slowest);
      }
      lock.lock();
    }
  }
  // Shutdown: what is left (the GPU is idle, the renderer drained its frames).
  std::vector<std::shared_ptr<Context>> left = std::move(doomed);
  doomed.clear();
  if (ctx) {
    left.push_back(std::move(ctx));
    ctx.reset();
  }
  if (next_ctx) {
    left.push_back(std::move(next_ctx));
    next_ctx.reset();
  }
  lock.unlock();
  for (const std::shared_ptr<Context>& old : left) {
    FreeContext(*old);
  }
}

void DlssBackend::Request(const BackendRequest& request) {
  Impl& i = *impl_;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed || i.status.state != BackendState::kAvailable) {
      return;
    }
    // The settings ask for DLSS: the pre-warm is over (its next steps would only delay the context
    // that is wanted, and make the frames wait for the worker).
    if (i.DropPrewarmLocked()) {
      REXLOG_INFO("{} warm-up stopped: DLSS was asked for ({} of {} steps done); the context asked "
                  "for is the worker's next job",
                  kPrefix, i.prewarm_done, i.prewarm_total);
    }
    if (!i.init_done) {
      // The first use of a DLSS that was not the choice at the start (D3D12): Streamline starts
      // now, on the worker; no context until it has.
      i.StartWorkerLocked();
      return;
    }
    if (i.want && i.want_request == request) {
      return;
    }
    if (i.RefusedLocked(request)) {
      // DLSS does not take this mode at these sizes (a refused context, or the size query): no
      // context is made for it, and Ready() stays false. The caller resolves another mode
      // (temporal.cpp ResolveDlssMode).
      return;
    }
  }
  // A new request. The side queue, list and fence once, the images of the warm-up for these sizes:
  // made here, on the render thread (Plume's create calls are its own), before the worker is told.
  i.EnsureKit();
  std::unique_ptr<WarmImages> images = i.MakeImages(request);
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed || i.stop) {
      return;
    }
    if (i.ctx && !(i.ctx->request == request)) {
      retire.push_back(std::move(i.ctx));
      i.ctx.reset();
      i.status.context_bytes = 0;
    }
    i.want = true;
    i.want_request = request;
    i.want_images = std::move(images);
  }
  i.cv.notify_all();
  i.RetireOutsideLock(retire);
}

bool DlssBackend::Ready(const BackendRequest& request) const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  return i.ctx && i.ctx->request == request && !i.status.failed;
}

void DlssBackend::AskModes(const ModeSizes& sizes) {
  Impl& i = *impl_;
  bool wake = false;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed || i.stop || (i.fit_asked && i.fit_ask == sizes)) {
      return;
    }
    i.fit_ask = sizes;
    i.fit_asked = true;
    // The worker answers once Streamline runs; this never starts it (Probe, Request and Prepare
    // do), so a PC that is not using DLSS pays nothing for the menu knowing.
    i.fit_dirty = i.UnknownModesLocked(sizes);
    i.PublishModesLocked();
    wake = i.fit_dirty;
  }
  if (wake) {
    i.cv.notify_all();
  }
}

ModeVerdicts DlssBackend::Modes(const ModeSizes& sizes) const {
  const Impl& i = *impl_;
  ModeVerdicts out;
  std::lock_guard lock(i.mu);
  for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
    const Impl::FitRecord* record =
        i.FindFitLocked(UpscaleMode(mode), sizes.output, sizes.render[mode]);
    if (record) {
      out.verdict[mode] = record->fits ? ModeVerdict::kRuns : ModeVerdict::kRefused;
      out.why[mode] = record->why;
    }
  }
  return out;
}

bool DlssBackend::Busy() const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  // The worker is making a context, or is about to (the wanted one is set before it wakes): either
  // way Streamline's mutex is, or is about to be, held for the seconds its first evaluation takes.
  return i.building.load() || i.WantNextLocked();
}

// CHECK: the prepared slot on both APIs: a second viewport alive beside the first (options, feature,
// slFreeResources of the old one after Commit), the second feature's build time ("made in the
// background in N ms": if it is far under the first's 8 s the plain interval of a DLSS to DLSS change
// is short), and that Busy() frames (plain, not jittered) show no shake.
// Make before break. The feature of a context takes seconds (NGX builds its network: 8 s on an RTX
// 5080), so a live change of mode makes the new context in the prepared slot, on the worker, with
// its own viewport, while the current one keeps dispatching; Commit swaps them between two frames.
void DlssBackend::Prepare(const BackendRequest& request) {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed || i.status.state != BackendState::kAvailable) {
      return;
    }
    // A live change to DLSS: the pre-warm is over (see Request).
    if (i.DropPrewarmLocked()) {
      REXLOG_INFO("{} warm-up stopped: DLSS was asked for ({} of {} steps done); the context asked "
                  "for is the worker's next job",
                  kPrefix, i.prewarm_done, i.prewarm_total);
    }
    if (!i.init_done) {
      // The first use of DLSS in a game that was not started with it chosen (D3D12): Streamline
      // starts now, on the worker (2 to 10 s); asked again every frame until it has.
      i.StartWorkerLocked();
      return;
    }
    if ((i.ctx && i.ctx->request == request) || (i.want && i.want_request == request)) {
      // The current slot is, or is being made as, the one asked for: nothing to prepare beside it.
      i.DropNextLocked(&retire);
    } else if (i.next_want && i.next_request == request) {
      return;
    } else {
      i.DropNextLocked(&retire);  // a different prepared one goes
    }
  }
  if (!retire.empty()) {
    i.RetireOutsideLock(retire);
  }
  {
    std::lock_guard lock(i.mu);
    if ((i.ctx && i.ctx->request == request) || (i.want && i.want_request == request) ||
        (i.next_want && i.next_request == request) || i.RefusedLocked(request)) {
      return;  // (a refused one: see Request)
    }
  }
  i.EnsureKit();
  std::unique_ptr<WarmImages> images = i.MakeImages(request);
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed || i.stop) {
      return;
    }
    i.next_want = true;
    i.next_request = request;
    i.next_images = std::move(images);
  }
  i.cv.notify_all();
}

bool DlssBackend::PreparedReady(const BackendRequest& request) const {
  const Impl& i = *impl_;
  std::lock_guard lock(i.mu);
  if (i.status.failed) {
    return false;
  }
  return (i.ctx && i.ctx->request == request) || (i.next_ctx && i.next_ctx->request == request);
}

bool DlssBackend::Commit(const BackendRequest& request) {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    if (i.status.failed) {
      return false;
    }
    if (i.ctx && i.ctx->request == request) {
      i.want = true;  // the current context already is it
      i.want_request = request;
      i.DropNextLocked(&retire);
    } else if (i.next_ctx && i.next_ctx->request == request) {
      if (i.ctx) {
        retire.push_back(std::move(i.ctx));
      }
      i.ctx = std::move(i.next_ctx);
      i.next_ctx.reset();
      i.next_want = false;
      i.next_images.reset();
      i.want = true;
      i.want_request = request;
      i.want_images.reset();
      i.status.context_bytes = i.ctx->bytes;
    } else {
      return false;
    }
  }
  i.RetireOutsideLock(retire);
  return true;
}

void DlssBackend::CancelPrepared() {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    i.DropNextLocked(&retire);
  }
  i.RetireOutsideLock(retire);
}

void DlssBackend::Release() {
  Impl& i = *impl_;
  std::vector<std::shared_ptr<Context>> retire;
  {
    std::lock_guard lock(i.mu);
    i.want = false;
    i.want_images.reset();
    if (i.ctx) {
      retire.push_back(std::move(i.ctx));
      i.ctx.reset();
    }
    i.DropNextLocked(&retire);
    i.status.context_bytes = 0;
  }
  i.RetireOutsideLock(retire);
}

void DlssBackend::Fail(const std::string& why) {
  Impl& i = *impl_;
  std::unique_lock lock(i.mu);
  i.FailLocked(lock, why, /*render_thread=*/true);
}

bool DlssBackend::PrewarmPending() const {
  return impl_->prewarm_pending.load(std::memory_order_acquire);
}

bool DlssBackend::PrewarmNext(UpscaleMode* mode) {
  Impl& i = *impl_;
  bool started_streamline = false;
  {
    std::lock_guard lock(i.mu);
    if (!i.prewarm_pending.load(std::memory_order_acquire)) {
      return false;
    }
    if (i.status.failed || i.stop) {
      i.DropPrewarmLocked();  // the failure was said by whoever made it
      return false;
    }
    if (!i.init_done) {
      // D3D12: Streamline's own start (slInit, then slSetD3DDevice with NGX: about 10 s) is the
      // pre-warm's first step, on the worker. (Only D3D12 arms a pre-warm: a Vulkan game gets DLSS
      // only when it is the start's choice, so it has no live switch to be quick.)
      if (!i.worker.joinable()) {
        i.StartWorkerLocked();
        started_streamline = true;
      }
    } else if (!i.prewarm_busy && !i.prewarm_modes.empty()) {
      *mode = i.prewarm_modes.front();
      return true;
    }
  }
  if (started_streamline) {
    REXLOG_INFO("{} warm-up: Streamline starts in the background (slInit and NGX, several seconds), "
                "then one feature per mode is made and freed, so that DLSS chosen from the menu "
                "is quick; nothing is kept in video memory",
                kPrefix);
  }
  return false;
}

// Render thread. One step: the four images for `request` (the worker needs them to make the
// feature) and a note to the worker. The worker makes the feature and frees it (JobLoop).
void DlssBackend::Prewarm(const BackendRequest& request) {
  Impl& i = *impl_;
  std::string skipped_why;
  uint32_t skipped_step = 0;
  uint32_t skipped_total = 0;
  {
    std::lock_guard lock(i.mu);
    if (!i.prewarm_pending.load(std::memory_order_acquire) || !i.init_done || i.prewarm_busy ||
        i.prewarm_modes.empty() || i.prewarm_modes.front() != request.mode || i.status.failed ||
        i.stop) {
      return;
    }
    // A mode DLSS does not take at these sizes has nothing to warm up: no images are made for it.
    const Impl::FitRecord* record =
        i.FindFitLocked(request.mode, request.output_size, request.render_size);
    if (record && !record->fits) {
      skipped_why = record->why;
      skipped_step = ++i.prewarm_done;
      skipped_total = i.prewarm_total;
      i.prewarm_modes.erase(i.prewarm_modes.begin());
      if (i.prewarm_modes.empty()) {
        i.prewarm_pending.store(false, std::memory_order_release);
      }
    }
  }
  if (skipped_step != 0) {
    REXLOG_INFO("{} warm-up {}/{}: {} at {}x{} to {}x{} skipped: {}", kPrefix, skipped_step,
                skipped_total, ModeInfoOf(Upscaler::kDlss, request.mode).name,
                request.render_size[0], request.render_size[1], request.output_size[0],
                request.output_size[1], skipped_why);
    return;
  }
  const std::string memory = i.PrewarmMemoryProblem(request);
  if (!memory.empty()) {
    // Not now, and not at the next frame either: a card this full gets no pre-warm (the first
    // live switch pays for itself, as before).
    bool pending = false;
    uint32_t done = 0;
    uint32_t total = 0;
    {
      std::lock_guard lock(i.mu);
      pending = i.DropPrewarmLocked();
      done = i.prewarm_done;
      total = i.prewarm_total;
    }
    if (pending) {
      REXLOG_INFO("{} warm-up stopped after {} of {} steps: {}", kPrefix, done, total, memory);
    }
    return;
  }
  i.EnsureKit();
  std::unique_ptr<WarmImages> images = i.MakeImages(request);
  bool first = false;
  bool unusable = false;
  uint32_t done = 0;
  uint32_t total = 0;
  {
    std::lock_guard lock(i.mu);
    if (!i.prewarm_pending.load(std::memory_order_acquire) || i.stop) {
      return;  // dropped while the images were made (a real request): they go with `images`
    }
    if (!images) {
      unusable = i.DropPrewarmLocked();
      done = i.prewarm_done;
      total = i.prewarm_total;
    } else {
      i.prewarm_modes.erase(i.prewarm_modes.begin());
      i.prewarm_job = true;
      i.prewarm_busy = true;
      i.prewarm_request = request;
      i.prewarm_images = std::move(images);
      if (!i.prewarm_start_logged) {
        i.prewarm_start_logged = true;
        i.prewarm_began = std::chrono::steady_clock::now();
        first = true;
      }
      total = i.prewarm_total;
    }
  }
  if (unusable) {
    REXLOG_INFO("{} warm-up stopped after {} of {} steps: the objects for a feature (a second queue, "
                "four images) could not be made",
                kPrefix, done, total);
    return;
  }
  if (first) {
    REXLOG_INFO("{} warm-up begins: {} steps (one feature per mode, the first at {}x{} to {}x{} and "
                "the others at their modes' sizes, each freed at once), off the render thread; a DLSS "
                "request ends it after the step in hand",
                kPrefix, total, request.render_size[0], request.render_size[1],
                request.output_size[0], request.output_size[1]);
  }
  i.cv.notify_all();
}

// One evaluation of `context`'s viewport recorded into `native_list`, on the render thread (a frame's
// dispatch) or the worker (Warmup): the frame token, the constants, and slEvaluateFeature with the
// four images as local tags. Streamline's lock is taken here. Empty when it was recorded, otherwise
// why not.
std::string DlssBackend::Impl::Evaluate(void* native_list, Context& context,
                                        const TemporalInputs& in, bool wait, bool* busy) {
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
    return "an input image has no native object, or Plume cannot describe it to Streamline";
  }

  // The images and their tags, local to this evaluation (no global tags, so no frame-based tagging
  // flag and nothing that waits for a present). The states are the ones the caller transitioned
  // them to; Streamline takes them back to those.
  // Streamline leaves the four images in the states declared here (the caller's barriers after the
  // dispatch assume it, as they do for FidelityFX). Not yet checked with the D3D12 debug layer's GPU-based validation or
  // Vulkan's synchronization validation.
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
  const bool reset = in.reset || !context.dispatched;
  sl::Constants consts;
  FillStaticConstants(consts);
  consts.cameraViewToClip = ToSl(in.view_to_clip);
  consts.clipToCameraView = ToSl(in.clip_to_view);
  consts.clipToPrevClip = ToSl(in.clip_to_prev_clip);
  consts.prevClipToClip = ToSl(in.prev_clip_to_clip);
  // The jitter is what the draws were moved by, in render pixels, +x right and +y down (FSR's
  // convention; the picture in motion is clean with it, 2026-11-06).
  consts.jitterOffset = sl::float2(in.jitter[0], in.jitter[1]);
  // The vectors are previous minus current in render pixels (+y down); scaled by one over the render
  // size they are in [-1, 1] as Streamline asks.
  consts.mvecScale = sl::float2(in.motion_scale[0] / float(in.render_size[0]),
                                in.motion_scale[1] / float(in.render_size[1]));
  consts.cameraPos = sl::float3(in.camera_position[0], in.camera_position[1], in.camera_position[2]);
  consts.cameraUp = sl::float3(in.camera_up[0], in.camera_up[1], in.camera_up[2]);
  consts.cameraRight = sl::float3(in.camera_right[0], in.camera_right[1], in.camera_right[2]);
  consts.cameraFwd = sl::float3(in.camera_forward[0], in.camera_forward[1], in.camera_forward[2]);
  consts.cameraNear = in.camera_near;
  consts.cameraFar = in.camera_far;
  consts.cameraFOV = in.camera_fov_y;
  consts.cameraAspectRatio = in.camera_aspect;
  consts.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

  Runtime& rt = Rt();
  std::unique_lock sl_lock(rt.mu, std::defer_lock);
  if (wait) {
    sl_lock.lock();
  } else if (!sl_lock.try_lock()) {
    if (building.load()) {
      // The worker is inside Streamline making a feature (the first evaluation of another viewport:
      // seconds): this frame goes without rather than waiting for it.
      *busy = true;
      return "Streamline is busy";
    }
    // A short hold (the options of a context, freeing an old one): waited for.
    sl_lock.lock();
  }
  if (!rt.initialized || rt.shut_down) {
    return "Streamline is not running";
  }
  sl::FrameToken* token = nullptr;
  uint32_t frame_index = ++frame_counter;
  if (!wait && g_chain_proxied.load(std::memory_order_acquire)) {
    // DLSS frame generation is on: Streamline's tokens are one ring and the same index gives the same
    // token, so the frame's Super Resolution, its constants, tags and Reflex markers share the
    // frame's one index, the latency id (a frame's dispatch only: the render thread reads it).
    if (const uint64_t id = latency::RenderFrameId(); id != 0) {
      frame_index = uint32_t(id);
    }
  }
  sl::Result result = rt.api.new_frame_token(token, &frame_index);
  if (result != sl::Result::eOk || !token) {
    return "slGetNewFrameToken failed: " + ResultText(result);
  }
  const sl::ViewportHandle viewport(context.viewport_id);
  result = rt.api.set_constants(consts, *token, viewport);
  if (result != sl::Result::eOk) {
    return "slSetConstants failed: " + ResultText(result);
  }
  const sl::BaseStructure* inputs[] = {&viewport, &color_tag, &depth_tag, &motion_tag, &output_tag};
  result = rt.api.evaluate(sl::kFeatureDLSS, *token, inputs, uint32_t(std::size(inputs)),
                           native_list);
  if (result != sl::Result::eOk) {
    return "slEvaluateFeature failed: " + ResultText(result);
  }
  if (!context.bytes_asked && rt.api.dlss_state) {
    context.bytes_asked = true;
    sl::DLSSState state;
    if (rt.api.dlss_state(viewport, state) == sl::Result::eOk) {
      context.bytes = state.estimatedVRAMUsageInBytes;
    }
  }
  return {};
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
  const bool reset = in.reset || !context->dispatched;
  bool busy = false;
  const std::string error = i.Evaluate(native_list, *context, in, /*wait=*/false, &busy);
  if (busy) {
    return false;  // nothing was recorded and nothing failed: this frame is plain (Busy())
  }
  if (!error.empty()) {
    return fail(error);
  }
  if (!context->created && !context->dispatched) {
    REXLOG_INFO("{} the feature was not made in the background: this first dispatch made it, which "
                "paused the game for several seconds",
                kPrefix);
  }
  if (!context->dispatched) {
    context->dispatched = true;
    if (context->bytes != 0) {
      std::lock_guard lock(i.mu);
      i.status.context_bytes = context->bytes;
    }
  }
  if (i.dispatches++ == 0) {
    REXLOG_INFO("{} first dispatch recorded: {}x{} to {}x{}, jitter {:+.3f}, {:+.3f}, near {:.3f}, "
                "far {:.1f}, field of view {:.2f} deg, reset {}, about {} MB",
                kPrefix, in.render_size[0], in.render_size[1], in.output_size[0], in.output_size[1],
                in.jitter[0], in.jitter[1], in.camera_near, in.camera_far,
                double(in.camera_fov_y) * 57.29577951308232, reset ? "yes" : "no",
                context->bytes >> 20);
  }
  return true;
}

void DlssBackend::Shutdown() {
  Impl& i = *impl_;
  {
    std::lock_guard lock(i.mu);
    i.stop = true;
    i.want = false;
    i.want_images.reset();
    i.next_want = false;
    i.next_images.reset();
    i.DropPrewarmLocked();
  }
  i.cv.notify_all();
  if (i.worker.joinable()) {
    // The worker cannot be interrupted inside Streamline: a start-up or a creation in progress
    // finishes first (the device is still alive, so it is safe), up to the seconds it takes.
    if (i.building.load()) {
      REXLOG_INFO("{} waiting for the feature that is being made in the background before shutting "
                  "down",
                  kPrefix);
    }
    i.worker.join();
  }
  std::vector<std::shared_ptr<Context>> left;
  {
    std::lock_guard lock(i.mu);
    left = std::move(i.doomed);
    i.doomed.clear();
    if (i.ctx) {
      left.push_back(std::move(i.ctx));
      i.ctx.reset();
    }
    if (i.next_ctx) {
      left.push_back(std::move(i.next_ctx));
      i.next_ctx.reset();
    }
  }
  if (i.probe_started) {
    // The GPU is idle (the renderer drained its frames): the resources go, then Streamline, before
    // the device does ("Call slShutdown() before destroying those instances"). The module stays
    // loaded to the end of the process. The swap chain's wrapper (D3D12) outlives it: Streamline
    // ignores the hook calls it gets after its shutdown, and Plume releases the wrapper with the
    // swap chain.
    g_swap_chain_pending.store(false, std::memory_order_release);
    g_reflex_ready.store(false, std::memory_order_release);
    dlss::ShutdownFrameGeneration();  // its resources and the plugin, before Streamline itself
    Runtime& rt = Rt();
    std::lock_guard lock(rt.mu);
    if (rt.initialized && !rt.shut_down) {
      for (const std::shared_ptr<Context>& old : left) {
        rt.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(old->viewport_id));
      }
      rt.api.shutdown();
      rt.initialized = false;
      rt.shut_down = true;
    }
  }
  left.clear();
  {
    // The side queue, list and fence go before the device does (the worker is gone).
    std::lock_guard lock(i.mu);
    i.kit.reset();
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
  // Only when DLSS is the saved choice at this start. The instance and the device have to be made
  // through Streamline's interposer for DLSS to run at all, and that costs several seconds at every
  // start (measured on an RTX 5080, 2026-11-07: the renderer's device up 9.7 s after the process
  // started instead of 0.5 s: slInit about 1.6 s, then NGX starting inside the hooked
  // vkCreateInstance/vkCreateDevice about 7.7 s, on the game's own thread, whether DLSS is used or
  // not). So a Vulkan game started with FSR or no upscaler pays none of it, and choosing DLSS from
  // its menu applies from the next start (Probe: "after restart"; the pause menu says RESTART and
  // offers it); D3D12 starts Streamline in the background and switches live.
  // CHECK: on an RTX PC, a Vulkan start with FSR or no upscaler has its device up about half a second
  // after the process starts and logs nothing here (the Probe's "listed for Vulkan" line follows);
  // a start with DLSS saved logs the "DLSS is the saved choice: Streamline started for Vulkan" line
  // below, with the device about 9.7 s in (LIVE2.txt).
  // DLSS frame generation counts as well (mcla_gpu_frame_generation = dlss): it needs the instance and
  // the device made through Streamline too, with its plugins' extensions, features and queues, so it is
  // a start-time choice on Vulkan like DLSS itself (a Vulkan game started without it lists it as
  // after-restart, dlss_frame_generation.h). On D3D12 neither needs a start-time decision.
  const bool dlss_chosen = ReadUpscalerSettings().upscaler == Upscaler::kDlss;
  const bool frame_gen_chosen = REXCVAR_GET(mcla_gpu_frame_generation) == "dlss";
  if (!dlss_chosen && !frame_gen_chosen) {
    return;
  }
#if !defined(LARECOMP_PLUME_STREAMLINE)
  REXLOG_INFO("{} DLSS or DLSS frame generation is chosen, but this build's Plume has no creation "
              "hooks for Streamline: it is not available on Vulkan in this build",
              kPrefix);
  (void)preferred_adapter;
#else
  // Streamline's vkCreateInstance and vkCreateDevice proxies add the extensions, features and
  // queues DLSS needs (Plume works with its individual feature structs, 2026-11-06). If the
  // instance or the device cannot be made through them, the game makes them again without it
  // (Device::Initialize, AbandonVulkanHook): DLSS is off for that session and says so.
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
  if (!LoadRuntimeLocked(rt, &why) ||
      !InitLocked(rt, sl::RenderAPI::eVulkan, frame_gen_chosen, &why)) {
    // The probe reports it (greyed as "needs setup" when the files are the problem).
    rt.failure = why;
    REXLOG_INFO("{} Streamline is not started for Vulkan, so the device is made without it: {}",
                kPrefix, why);
    return;
  }
  void* get_instance_proc_addr =
      reinterpret_cast<void*>(GetProcAddress(rt.interposer, "vkGetInstanceProcAddr"));
  if (!get_instance_proc_addr) {
    rt.failure = "sl.interposer.dll does not export vkGetInstanceProcAddr";
    REXLOG_INFO("{} Streamline is not started for Vulkan, so the device is made without it: {}",
                kPrefix, rt.failure);
    return;
  }
  rt.vk_get_instance_proc_addr = get_instance_proc_addr;
  rt.vk_hook = true;
  REXLOG_INFO("{} {} is the saved choice: Streamline started for Vulkan before the device. Plume "
              "loads Vulkan through its vkGetInstanceProcAddr, whose vkCreateInstance and "
              "vkCreateDevice add what DLSS needs (the start takes several seconds longer for "
              "it: Streamline's start and NGX's, on this thread)",
              kPrefix,
              dlss_chosen && frame_gen_chosen ? "DLSS with frame generation"
                                              : (dlss_chosen ? "DLSS" : "DLSS frame generation"));
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

// ---------------------------------------------------------------------------
// D3D12: Streamline sees the presents
// ---------------------------------------------------------------------------
// The common plugin's presentCommon (its frame counter, its deferred freeing of resources, the
// clearing of its caches at a resize) runs from the Present and ResizeBuffers of Streamline's
// wrapper of the swap chain, which the manual hooking guide has the application make with
// slUpgradeInterface "immediately after creating the swap chain". That cannot be done here:
//   - the wrapper asks the plugin manager for its hook lists in every call (Present, GetBuffer,
//     GetCurrentBackBufferIndex, ResizeBuffers). Before slSetD3DDevice has finished starting the
//     plugins (about 8 s of NGX on an RTX 5080, after about 2 s of slInit) such a call starts them
//     itself, a second time and racing the worker's start, or logs an error at every call.
//   - the swap chain is made at the start of the game, before anything else of it can run, and
//     Streamline's start must not delay that by ten seconds (nothing runs in parallel with it).
// So the swap chain goes native and Plume offers it at every present (backend_d3d12.cpp, Plume's
// upgradeSwapChainAtPresent) until the worker has published the end of the start; the next
// present then swaps the wrapper in. The wrapper passes everything to the swap chain it wraps, and
// the back buffers Plume holds are the swap chain's own, so nothing else changes.
// Run 2026-11-06: sl.log says "Upgrading IDXGISwapChain to use SL proxy" and no longer logs
// "presentCommon() was not observed"; frame times unchanged; no refcount warning at exit. Not yet
// tested: a swap chain resize (window size, fullscreen) and HDR through the wrapper (SetColorSpace1 and
// SetHDRMetaData are passed through).

namespace dlss {

bool SwapChainUpgradePending() {
  // Not over a chain that is Streamline's own proxy already (frame generation made it through the
  // proxy factory): its calls would be a second wrapper's.
  if (g_chain_proxied.load(std::memory_order_acquire)) {
    return false;
  }
  return g_swap_chain_pending.load(std::memory_order_acquire) ||
         g_swap_chain_again.load(std::memory_order_acquire);
}

// CHECK: not run. After an HDR switch on D3D12 with Streamline: sl.log says "Upgrading IDXGISwapChain
// to use SL proxy" again and the game log "Streamline's swap chain wrapper is in" a second time,
// no "presentCommon() was not observed", and no refcount warning at exit (the old wrapper was
// released with its chain).
void SwapChainRecreated() {
  // Before the first offer the pending flag does it all (Streamline is still starting, or the
  // first present will offer the chain that exists then). A proxy chain of Streamline's own
  // (frame generation) is not offered: when it goes, NoteSwapChainProxied(false) and the call for
  // the plain chain that replaces it make the offer.
  if (g_chain_proxied.load(std::memory_order_acquire)) {
    return;
  }
  if (g_swap_chain_offered.load(std::memory_order_acquire)) {
    g_swap_chain_again.store(true, std::memory_order_release);
  }
}

bool UpgradeSwapChain(void** swap_chain) {
  if (!swap_chain || !*swap_chain || g_chain_proxied.load(std::memory_order_acquire) ||
      (!g_swap_chain_pending.load(std::memory_order_acquire) &&
       !g_swap_chain_again.load(std::memory_order_acquire))) {
    return false;
  }
  Runtime& rt = Rt();
  // A present never waits for Streamline: the worker holds the lock for the seconds a feature's
  // creation takes, so the next present asks again. Only the frames' own evaluations count
  // towards Streamline's "evaluated without a present" warning (60 of them), and they start after
  // the creation, at whose first present the swap chain is taken.
  std::unique_lock lock(rt.mu, std::try_to_lock);
  if (!lock.owns_lock()) {
    return false;
  }
  g_swap_chain_pending.store(false, std::memory_order_release);  // once per chain
  if (g_swap_chain_again.exchange(false, std::memory_order_acq_rel)) {
    rt.swap_chain_tried = false;  // a new chain: its own offer (the old wrapper went with its chain)
  }
  if (!rt.initialized || rt.shut_down || rt.swap_chain_tried || !rt.api.upgrade_interface) {
    return false;
  }
  rt.swap_chain_tried = true;
  g_swap_chain_offered.store(true, std::memory_order_release);
  void* wrapper = *swap_chain;
  const sl::Result result = rt.api.upgrade_interface(&wrapper);
  if (result != sl::Result::eOk || !wrapper) {
    REXLOG_WARN("{} Streamline did not take the swap chain ({}): it does not see the presents, "
                "which only costs it memory across window resizes; DLSS itself runs",
                kPrefix, ResultText(result));
    return false;
  }
  if (wrapper == *swap_chain) {
    REXLOG_WARN("{} Streamline gave the swap chain back unchanged (its interposer is disabled): it "
                "does not see the presents",
                kPrefix);
    return false;
  }
  *swap_chain = wrapper;
  REXLOG_INFO("{} Streamline's swap chain wrapper is in: it sees every present from now on", kPrefix);
  return true;
}

}  // namespace dlss

// ---------------------------------------------------------------------------
// Frame generation and Reflex through Streamline
// ---------------------------------------------------------------------------
// The neutral functions of temporal_dlss.h's second half (dlss_frame_generation.h and
// latency_streamline.cpp call them). Everything that needs Streamline's lock takes it the way a
// frame's dispatch does: tried first, and when the worker holds it for a feature's creation
// (g_building) the answer is "busy" and the caller asks again at the next frame; a short hold
// (options, freeing a context) is waited for.

namespace dlss {

FrameGenerationFacts GetFrameGenerationFacts() {
  std::lock_guard lock(g_fg_mutex);
  return g_fg_facts;
}

bool FrameGenerationFilesPresent(std::string* missing) {
  const std::wstring dir = ExeDirectory();
  std::string list = MissingFiles(dir);
  for (const wchar_t* file : kFrameGenFiles) {
    if (dir.empty() || !FileExists(dir + file)) {
      list += (list.empty() ? "" : ", ") + Utf8(file);
    }
  }
  if (missing) {
    *missing = list;
  }
  return list.empty();
}

void NoteSwapChainProxied(bool proxied) {
  g_chain_proxied.store(proxied, std::memory_order_release);
}

#if defined(LARECOMP_HAS_DLSS_G)

namespace {

// Streamline's lock for a call of the render thread's: false (and not held) when the worker is making
// a feature.
bool LockForFrame(Runtime& rt, std::unique_lock<std::mutex>& lock) {
  lock = std::unique_lock<std::mutex>(rt.mu, std::try_to_lock);
  if (lock.owns_lock()) {
    return true;
  }
  if (g_building.load()) {
    return false;
  }
  lock.lock();
  return true;
}

// The frame token of `index` (Streamline keeps a ring of a few and gives the same token again for the
// same index, so every call of a frame, from any thread, with the frame's index names one frame).
sl::FrameToken* TokenFor(Runtime& rt, uint32_t index) {
  sl::FrameToken* token = nullptr;
  uint32_t value = index;
  if (rt.api.new_frame_token(token, &value) != sl::Result::eOk) {
    return nullptr;
  }
  return token;
}

// The last options asked for (under Runtime::mu): slDLSSGGetState wants them for the video memory
// estimate.
sl::DLSSGOptions g_last_options;

constexpr uint32_t kViewport = 0;  // frame generation's own viewport: Super Resolution's contexts start at 1

}  // namespace

LoadResult LoadFrameGeneration(bool loaded, std::string* why) {
  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    return LoadResult::kBusy;
  }
  if (!rt.initialized || rt.shut_down || !rt.frame_gen_asked || !rt.api.set_feature_loaded) {
    *why = "Streamline does not run with the frame generation plugins";
    return LoadResult::kFailed;
  }
  if (rt.frame_gen_loaded == loaded) {
    return LoadResult::kDone;
  }
  if (loaded && !GetFrameGenerationFacts().supported) {
    *why = "Streamline said frame generation is not supported here";
    return LoadResult::kFailed;
  }
  const sl::Result result = rt.api.set_feature_loaded(sl::kFeatureDLSS_G, loaded);
  if (result != sl::Result::eOk) {
    *why = std::format("slSetFeatureLoaded({}) failed: {}", loaded ? "true" : "false",
                       ResultText(result));
    return LoadResult::kFailed;
  }
  rt.frame_gen_loaded = loaded;
  if (loaded) {
    // The plugin starts again with the device it was given: its functions are taken again.
    void* set_options = nullptr;
    void* get_state = nullptr;
    const sl::Result a = rt.api.get_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_options);
    const sl::Result b = rt.api.get_function(sl::kFeatureDLSS_G, "slDLSSGetState", get_state);
    if (a != sl::Result::eOk || b != sl::Result::eOk || !set_options || !get_state) {
      *why = "the frame generation plugin's functions are missing after it was loaded";
      rt.api.set_feature_loaded(sl::kFeatureDLSS_G, false);
      rt.frame_gen_loaded = false;
      return LoadResult::kFailed;
    }
    rt.api.dlssg_set_options = reinterpret_cast<PFun_slDLSSGSetOptions*>(set_options);
    rt.api.dlssg_get_state = reinterpret_cast<PFun_slDLSSGGetState*>(get_state);
    g_last_options = sl::DLSSGOptions{};
  }
  return LoadResult::kDone;
}

bool UpgradeFactory(void** factory) {
  if (!factory || !*factory) {
    return false;
  }
  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    return false;
  }
  if (!rt.initialized || rt.shut_down || !rt.api.upgrade_interface || !rt.frame_gen_loaded) {
    return false;
  }
  void* proxy = *factory;
  const sl::Result result = rt.api.upgrade_interface(&proxy);
  if (result != sl::Result::eOk || !proxy || proxy == *factory) {
    REXLOG_WARN("{} Streamline did not give a proxy of the DXGI factory ({}): the swap chain is made "
                "without it, and frame generation cannot present",
                kPrefix, ResultText(result));
    return false;
  }
  *factory = proxy;
  return true;
}

bool SetFrameGenerationOptions(const FrameGenerationOptions& o, std::string* why, bool* busy) {
  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    *busy = true;
    return false;
  }
  if (!rt.initialized || rt.shut_down || !rt.frame_gen_loaded || !rt.api.dlssg_set_options) {
    *why = "the frame generation plugin is not loaded";
    return false;
  }
  sl::DLSSGOptions options;
  options.mode = o.on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  options.numFramesToGenerate = std::max<uint32_t>(o.frames_to_generate, 1u);
  // The plugin keeps its memory while off (the pause menu, a loading screen), so that switching back
  // is not a stutter; the whole plugin is unloaded by LoadFrameGeneration when the setting goes off.
  options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
  options.numBackBuffers = o.back_buffers;
  options.colorWidth = o.color_size[0];
  options.colorHeight = o.color_size[1];
  options.mvecDepthWidth = o.mvec_depth_size[0];
  options.mvecDepthHeight = o.mvec_depth_size[1];
  options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
  const sl::Result result = rt.api.dlssg_set_options(sl::ViewportHandle(kViewport), options);
  if (result != sl::Result::eOk) {
    *why = "slDLSSGSetOptions failed: " + ResultText(result);
    return false;
  }
  g_last_options = options;
  return true;
}

bool TagFrameGeneration(void* native_list, const FrameGenerationInputs& in, std::string* why,
                        bool* busy) {
  const backend::NativeImage depth_native =
      backend::NativeImageOf(in.depth.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage motion_native =
      backend::NativeImageOf(in.motion.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage hudless_native =
      backend::NativeImageOf(in.hudless.texture, backend::ImageUse::kComputeRead);
  const backend::NativeImage ui_native =
      backend::NativeImageOf(in.ui.texture, backend::ImageUse::kComputeRead);
  if (!depth_native.complete || !motion_native.complete || !hudless_native.complete ||
      !ui_native.complete) {
    *why = "an input image has no native object, or Plume cannot describe it to Streamline";
    return false;
  }
  sl::Resource depth_res = MakeSlResource(in.depth, depth_native);
  sl::Resource motion_res = MakeSlResource(in.motion, motion_native);
  sl::Resource hudless_res = MakeSlResource(in.hudless, hudless_native);
  sl::Resource ui_res = MakeSlResource(in.ui, ui_native);
  const sl::Extent render_extent = MakeExtent(in.render_size[0], in.render_size[1]);
  const sl::Extent back_extent = MakeExtent(in.back_size[0], in.back_size[1]);
  // Valid until the present: the four are made in this frame's list, are not written again before
  // the present (the next frame's work is on the same queue, which Streamline holds until its own
  // work on them is done: DLSSGQueueParallelismMode::eBlockPresentingClientQueue) and are left in the
  // compute read state they were given.
  const sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilPresent;
  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&depth_res, sl::kBufferTypeDepth, lifecycle, &render_extent),
      sl::ResourceTag(&motion_res, sl::kBufferTypeMotionVectors, lifecycle, &render_extent),
      sl::ResourceTag(&hudless_res, sl::kBufferTypeHUDLessColor, lifecycle, &back_extent),
      sl::ResourceTag(&ui_res, sl::kBufferTypeUIColorAndAlpha, lifecycle, &back_extent),
  };

  sl::Constants consts;
  FillStaticConstants(consts);
  consts.cameraViewToClip = ToSl(in.view_to_clip);
  consts.clipToCameraView = ToSl(in.clip_to_view);
  consts.clipToPrevClip = ToSl(in.clip_to_prev_clip);
  consts.prevClipToClip = ToSl(in.prev_clip_to_clip);
  consts.jitterOffset = sl::float2(in.jitter[0], in.jitter[1]);
  // The vectors are render pixels (the temporal system's motion pass); over the render size they are
  // in [-1, 1] as Streamline asks.
  consts.mvecScale = sl::float2(1.0f / float(std::max(in.render_size[0], 1u)),
                                1.0f / float(std::max(in.render_size[1], 1u)));
  consts.cameraPos = sl::float3(in.camera_position[0], in.camera_position[1], in.camera_position[2]);
  consts.cameraUp = sl::float3(in.camera_up[0], in.camera_up[1], in.camera_up[2]);
  consts.cameraRight = sl::float3(in.camera_right[0], in.camera_right[1], in.camera_right[2]);
  consts.cameraFwd = sl::float3(in.camera_forward[0], in.camera_forward[1], in.camera_forward[2]);
  consts.cameraNear = in.camera_near;
  consts.cameraFar = in.camera_far;
  consts.cameraFOV = in.camera_fov_y;
  consts.cameraAspectRatio = in.camera_aspect;
  consts.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    *busy = true;
    return false;
  }
  if (!rt.initialized || rt.shut_down || !rt.frame_gen_loaded || !rt.api.set_tag_for_frame) {
    *why = "the frame generation plugin is not loaded";
    return false;
  }
  sl::FrameToken* token = TokenFor(rt, in.frame_index);
  if (!token) {
    *why = "slGetNewFrameToken failed";
    return false;
  }
  const sl::ViewportHandle viewport(kViewport);
  sl::Result result = rt.api.set_constants(consts, *token, viewport);
  if (result != sl::Result::eOk) {
    *why = "slSetConstants failed: " + ResultText(result);
    return false;
  }
  result = rt.api.set_tag_for_frame(*token, viewport, tags, uint32_t(std::size(tags)), native_list);
  if (result != sl::Result::eOk) {
    *why = "slSetTagForFrame failed: " + ResultText(result);
    return false;
  }
  return true;
}

bool ReadFrameGenerationState(FrameGenerationState* out, bool request_vram_estimate,
                              std::string* why, bool* busy) {
  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    *busy = true;
    return false;
  }
  if (!rt.initialized || rt.shut_down || !rt.frame_gen_loaded || !rt.api.dlssg_get_state) {
    *why = "the frame generation plugin is not loaded";
    return false;
  }
  sl::DLSSGState state;
  sl::DLSSGOptions with_estimate = g_last_options;
  with_estimate.flags |= sl::DLSSGFlags::eRequestVRAMEstimate;
  const sl::Result result = rt.api.dlssg_get_state(sl::ViewportHandle(kViewport), state,
                                                   request_vram_estimate ? &with_estimate : nullptr);
  if (result != sl::Result::eOk) {
    *why = "slDLSSGGetState failed: " + ResultText(result);
    return false;
  }
  const uint32_t status = uint32_t(state.status);
  out->ok = true;
  out->frames_presented = state.numFramesActuallyPresented;
  out->max_frames_generated = state.numFramesToGenerateMax;
  out->min_size = state.minWidthOrHeight;
  out->vram_bytes = state.estimatedVRAMUsageInBytes;
  out->resolution_too_low = (status & uint32_t(sl::DLSSGStatus::eFailResolutionTooLow)) != 0;
  out->reflex_missing = (status & uint32_t(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) != 0;
  out->hdr_format_unsupported = (status & uint32_t(sl::DLSSGStatus::eFailHDRFormatNotSupported)) != 0;
  out->constants_invalid = (status & uint32_t(sl::DLSSGStatus::eFailCommonConstantsInvalid)) != 0;
  out->back_buffer_index_not_asked =
      (status & uint32_t(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)) != 0;
  return true;
}

void ShutdownFrameGeneration() {
  Runtime& rt = Rt();
  std::lock_guard lock(rt.mu);
  if (!rt.initialized || rt.shut_down || !rt.frame_gen_loaded) {
    return;
  }
  if (rt.api.free_resources) {
    rt.api.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle(kViewport));
  }
  if (rt.api.set_feature_loaded) {
    rt.api.set_feature_loaded(sl::kFeatureDLSS_G, false);
  }
  rt.frame_gen_loaded = false;
}

namespace reflex {

bool Available() {
  return g_reflex_ready.load(std::memory_order_acquire);
}

bool SetMode(bool on, bool boost) {
  if (!g_reflex_ready.load(std::memory_order_acquire)) {
    return false;
  }
  Runtime& rt = Rt();
  std::unique_lock<std::mutex> lock;
  if (!LockForFrame(rt, lock)) {
    return false;  // the latency reducer asks again a second later
  }
  if (!rt.initialized || rt.shut_down || !rt.api.reflex_set_options) {
    return false;
  }
  sl::ReflexOptions options;
  options.mode = on ? (boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency)
                    : sl::ReflexMode::eOff;
  options.frameLimitUs = 0;  // the game's own limiter keeps the cap (latency.h)
  return rt.api.reflex_set_options(options) == sl::Result::eOk;
}

bool Sleep(uint32_t frame) {
  if (!g_reflex_ready.load(std::memory_order_acquire)) {
    return false;
  }
  // Thread safe in Streamline, and it blocks until the driver says the frame may start: not under the
  // lock a feature's creation holds.
  Runtime& rt = Rt();
  sl::FrameToken* token = TokenFor(rt, frame);
  return token && rt.api.reflex_sleep && rt.api.reflex_sleep(*token) == sl::Result::eOk;
}

void SetMarker(ReflexMarker marker, uint32_t frame) {
  if (!g_reflex_ready.load(std::memory_order_acquire)) {
    return;
  }
  sl::PCLMarker mapped = sl::PCLMarker::eSimulationStart;
  switch (marker) {
    case ReflexMarker::kSimulationStart: mapped = sl::PCLMarker::eSimulationStart; break;
    case ReflexMarker::kSimulationEnd: mapped = sl::PCLMarker::eSimulationEnd; break;
    case ReflexMarker::kRenderSubmitStart: mapped = sl::PCLMarker::eRenderSubmitStart; break;
    case ReflexMarker::kRenderSubmitEnd: mapped = sl::PCLMarker::eRenderSubmitEnd; break;
    case ReflexMarker::kPresentStart: mapped = sl::PCLMarker::ePresentStart; break;
    case ReflexMarker::kPresentEnd: mapped = sl::PCLMarker::ePresentEnd; break;
    case ReflexMarker::kInputSample:
      return;  // Streamline's PC latency plugin has no input sample marker (it was removed)
  }
  Runtime& rt = Rt();
  sl::FrameToken* token = TokenFor(rt, frame);
  if (token && rt.api.pcl_set_marker) {
    rt.api.pcl_set_marker(mapped, *token);
  }
}

}  // namespace reflex

#else  // no frame generation plugins in this build

LoadResult LoadFrameGeneration(bool, std::string* why) {
  *why = "this build has no DLSS frame generation";
  return LoadResult::kFailed;
}
bool UpgradeFactory(void**) {
  return false;
}
bool SetFrameGenerationOptions(const FrameGenerationOptions&, std::string* why, bool*) {
  *why = "this build has no DLSS frame generation";
  return false;
}
bool TagFrameGeneration(void*, const FrameGenerationInputs&, std::string* why, bool*) {
  *why = "this build has no DLSS frame generation";
  return false;
}
bool ReadFrameGenerationState(FrameGenerationState*, bool, std::string* why, bool*) {
  *why = "this build has no DLSS frame generation";
  return false;
}
void ShutdownFrameGeneration() {}
namespace reflex {
bool Available() {
  return false;
}
bool SetMode(bool, bool) {
  return false;
}
bool Sleep(uint32_t) {
  return false;
}
void SetMarker(ReflexMarker, uint32_t) {}
}  // namespace reflex

#endif

}  // namespace dlss

}  // namespace mcla::gpu
