// Present probe for frame_pacing_trace: which host Present put which frame on
// screen, and at which display refresh.
//
// The rest of the trace knows when the game simulates a frame and when the
// native runtime hands it to rex::ui::Presenter. What decides what the player
// sees comes after that, inside the SDK's D3D12Presenter, behind a swap chain
// it keeps private: when it calls Present, how often, and which of those
// Presents DWM (or the display, in independent flip) actually shows.
//
// DXGI reports the last part itself. IDXGISwapChain::GetFrameStatistics names
// the last Present that reached the screen (its PresentCount, the same count
// GetLastPresentCount returns after each Present) and the refresh it was shown
// at, with the QPC time of a refresh to anchor the refresh count to the clock.
// So the factory larecomp creates for the no-command-processor mode gets its
// CreateSwapChainForHwnd entry patched before the presenter creates its swap
// chain; that hook patches the new swap chain's Present, and every Present then
// writes one line with its count and one with the newest frame statistics.
//
// Trace only: installed when frame_pacing_trace is already on at startup. The
// patched entries are shared by every DXGI object of the same class in the
// process, so the hooks only add a branch when the trace is off again.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "frame_pacing.h"

#include <atomic>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_4.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, frame_pacing_trace);

#if defined(_WIN32)

namespace {

// COM vtable slots. IUnknown takes 0-2 and IDXGIObject 3-6; IDXGIFactory
// 7-11, IDXGIFactory1 12-13, then IDXGIFactory2 starts with
// IsWindowedStereoEnabled at 14. IDXGIDeviceSubObject::GetDevice is 7 on the
// swap chain side, so IDXGISwapChain::Present is 8.
constexpr size_t kFactoryCreateSwapChainForHwnd = 15;
constexpr size_t kSwapChainPresent = 8;

using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

CreateSwapChainForHwndFn g_create_swap_chain = nullptr;
PresentFn g_present = nullptr;

uint64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}

// Swaps one vtable entry for `hook`, returning what was there. The tables live
// in read-only data, so the page is opened for the one write.
void* PatchSlot(void* object, size_t index, void* hook) {
  void** vtable = *reinterpret_cast<void***>(object);
  void** slot = &vtable[index];
  if (*slot == hook) {
    return nullptr;  // already ours
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    return nullptr;
  }
  void* original = *slot;
  *slot = hook;
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  return original;
}

// The frame statistics only change when another Present reaches the screen,
// so a line is written only when they do.
std::atomic<uint32_t> g_last_stats_present{UINT32_MAX};
std::atomic<uint32_t> g_last_stats_sync{UINT32_MAX};
std::atomic<uint32_t> g_presents_since_media{0};

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swap_chain, UINT sync_interval,
                                        UINT flags) {
  const uint64_t enter = Qpc();
  const HRESULT hr = g_present(swap_chain, sync_interval, flags);
  if (!mc::pacing::TraceEnabled()) {
    return hr;
  }
  const uint64_t exit = Qpc();
  UINT present_count = 0;
  swap_chain->GetLastPresentCount(&present_count);
  mc::pacing::TraceWrite("Q,%llu,%llu,%u,%u,%u,%lu,%08lX\n", (unsigned long long)enter,
                         (unsigned long long)exit, present_count, sync_interval, flags,
                         (unsigned long)GetCurrentThreadId(), (unsigned long)hr);

  DXGI_FRAME_STATISTICS stats = {};
  const HRESULT stats_hr = swap_chain->GetFrameStatistics(&stats);
  if (SUCCEEDED(stats_hr)) {
    const uint32_t last_present = g_last_stats_present.exchange(stats.PresentCount);
    const uint32_t last_sync = g_last_stats_sync.exchange(stats.SyncRefreshCount);
    if (stats.PresentCount != last_present || stats.SyncRefreshCount != last_sync) {
      mc::pacing::TraceWrite("S,%llu,%u,%u,%u,%lld\n", (unsigned long long)exit,
                             stats.PresentCount, stats.PresentRefreshCount,
                             stats.SyncRefreshCount, (long long)stats.SyncQPCTime.QuadPart);
    }
  } else {
    static std::atomic<uint32_t> errors{0};
    if (errors.fetch_add(1) < 64) {
      mc::pacing::TraceWrite("S,%llu,ERR,%08lX\n", (unsigned long long)exit,
                             (unsigned long)stats_hr);
    }
  }

  // Whether DWM composes the window or the display flips to it directly. It
  // decides whether a Present can be shown at any refresh or only after the
  // compositor has taken it, so it is part of what the numbers above mean.
  if (g_presents_since_media.fetch_add(1) % 240 == 0) {
    IDXGISwapChainMedia* media = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&media))) && media) {
      DXGI_FRAME_STATISTICS_MEDIA media_stats = {};
      const HRESULT media_hr = media->GetFrameStatisticsMedia(&media_stats);
      mc::pacing::TraceWrite("M,%llu,%08lX,%d,%u\n", (unsigned long long)exit,
                             (unsigned long)media_hr, int(media_stats.CompositionMode),
                             media_stats.ApprovedPresentDuration);
      media->Release();
    }
  }
  return hr;
}

void HookSwapChain(IUnknown* object) {
  if (!object) {
    return;
  }
  void* original = PatchSlot(object, kSwapChainPresent, reinterpret_cast<void*>(&HookedPresent));
  if (original && !g_present) {
    g_present = reinterpret_cast<PresentFn>(original);
  }
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
  const HRESULT hr = g_create_swap_chain(factory, device, hwnd, desc, fullscreen_desc,
                                         restrict_to_output, swap_chain);
  if (SUCCEEDED(hr) && swap_chain && *swap_chain) {
    HookSwapChain(*swap_chain);
    // The presenter keeps an IDXGISwapChain3. DXGI hands every swap chain
    // interface out as one object today, but should that pointer ever differ,
    // its table is the one Present goes through.
    IDXGISwapChain3* swap_chain3 = nullptr;
    if (SUCCEEDED((*swap_chain)->QueryInterface(IID_PPV_ARGS(&swap_chain3))) && swap_chain3) {
      HookSwapChain(swap_chain3);
      swap_chain3->Release();
    }
    if (desc) {
      mc::pacing::TraceWrite("#swapchain,%llu,%u,%u,buffers=%u,effect=%d,flags=%08X,scaling=%d\n",
                             (unsigned long long)Qpc(), desc->Width, desc->Height,
                             desc->BufferCount, int(desc->SwapEffect), desc->Flags,
                             int(desc->Scaling));
    }
    REXLOG_INFO("[frame_pacing] present probe on swap chain {:p} ({}x{})",
                static_cast<void*>(*swap_chain), desc ? desc->Width : 0u,
                desc ? desc->Height : 0u);
  }
  return hr;
}

}  // namespace

namespace mc::pacing {

void InstallPresentProbe(void* dxgi_factory2) {
  if (!REXCVAR_GET(frame_pacing_trace) || !dxgi_factory2 || g_create_swap_chain) {
    return;
  }
  void* original = PatchSlot(dxgi_factory2, kFactoryCreateSwapChainForHwnd,
                             reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd));
  if (!original) {
    REXLOG_WARN("[frame_pacing] present probe: could not patch CreateSwapChainForHwnd");
    return;
  }
  g_create_swap_chain = reinterpret_cast<CreateSwapChainForHwndFn>(original);
  REXLOG_INFO("[frame_pacing] present probe installed on the presenter's DXGI factory");
}

}  // namespace mc::pacing

#else

namespace mc::pacing {
void InstallPresentProbe(void*) {}
}  // namespace mc::pacing

#endif  // _WIN32

#endif  // REXGLUE_HAS_XEO3_TARGET
