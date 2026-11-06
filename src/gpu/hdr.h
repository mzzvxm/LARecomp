#pragma once
// ===========================================================================
// MCLA GPU renderer -- HDR10 output, the settings and the display's levels
// ===========================================================================
// Stage 1 of the HDR work (the swap chain and the final pass). With HDR on, the
// window's swap chain is HDR10 (R10G10B10A2, Rec.2020, SMPTE ST 2084) and the
// passes that write it -- the present, FSR1's last pass and the overlay --
// end with hlsl/hdr_output.hlsli: the SDR picture they made is decoded as
// Windows shows SDR on an HDR desktop, placed at paper white in nits, rolled
// off towards the peak, and written as PQ. The game's own display gamma table
// is applied first, as in SDR, so DISPLAY GAMMA means the same. The picture is
// the SDR one placed at paper white: nothing goes above it yet (HDR-PLAN.md).
//
// The setting (mcla_gpu_hdr: off / auto / on) is read once, at start: the swap
// chain's format cannot change without making the pipelines that draw into it
// again, so it needs a restart. auto follows Windows' HDR switch for the
// display the window is on; on tries even when Windows says it is off. If the
// swap chain cannot be made HDR10 (no HDR10 colour space on the output, or a
// Vulkan surface without HDR10_ST2084) the SDR one is made, with one log line
// saying why.
//
// The levels (paper white, peak) come from the settings or, at 0, from what
// Windows reports for the display the window is on: DXGI's IDXGIOutput6
// (GetDesc1: the colour space, the peak) and DisplayConfig (the SDR content
// brightness). The display is found from the window's HMONITOR over every
// adapter's outputs, not from the swap chain, so it is right when the GPU that
// renders is not the one the display hangs off (an iGPU rendering, the RTX
// driving the monitor). A thread reads it once a second while HDR is wanted.
//
// Threading: Start/Stop on the lifecycle thread; Wanted/SetActive in between on
// it; Levels/Metadata on the render thread; Active and Auto on any thread.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface.h>

namespace mcla::gpu {

// What the HDR shaders take, in the push constants of the present pass, FSR1's last pass and the
// overlay (hlsl/hdr_output.hlsli HdrLevels).
struct HdrLevels {
  float white = 200.0f;  // SDR white (paper white), nits
  float peak = 1000.0f;  // nits
  float knee = 750.0f;   // the roll-off starts here, nits
  uint32_t flags = 0;
};
// HdrLevels::flags. Bit 0 (a 2.2 power instead of the sRGB curve) is in the shaders and not set.
inline constexpr uint32_t kHdrFlagTestPattern = 1u << 1;
// The present's push constants only (never in HdrLevels): the source holds code values that may
// exceed 1 (the HdrScene's float image, hdr_scene.h), so the display's gamma table is read as
// before up to 1 and extended above 1 by the slope of its last step (hlsl/present_ps.hlsl Ramp).
inline constexpr uint32_t kHdrFlagExtendedCodes = 1u << 2;

namespace hdr {

// Lifecycle thread, before the swap chain: reads the settings and, when HDR is wanted, reads the
// display once and starts the thread that reads it again each second. `window` is the HWND
// (Windows); anywhere else no display is known.
void Start(plume::RenderWindow window);
// Lifecycle thread: stops the thread. Safe to call more than once.
void Stop();

// Lifecycle thread, after Start and before the swap chain: whether to try an HDR10 swap chain.
// Logs the reason when not.
bool Wanted();
// After the swap chain: whether it is HDR10. Fixes the levels' behaviour from now on.
void SetActive(bool active);

// Any thread: the swap chain is HDR10.
bool Active();

// Render thread, per frame: the levels the settings and the display give.
HdrLevels Levels();
// Render thread: what the display is told about the content (the peak as its brightest light).
plume::RenderHdrMetadata Metadata();

// What the settings' AUTO stands for right now: the display's report, else the fallbacks.
// Any thread; `from_display` is false while nothing was read (the numbers are the fallbacks).
struct AutoLevels {
  float paper_white = 200.0f;
  float peak = 1000.0f;
  bool from_display = false;
};
AutoLevels Auto();

}  // namespace hdr
}  // namespace mcla::gpu
