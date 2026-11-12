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
// the SDR one placed at paper white: stage 2 (frame_layers.h) keeps the UI as a layer of its own and
// stage 3 (hdr_tonemap.h) puts the scene's lights above paper white (HDR-PLAN.md).
//
// The setting (mcla_gpu_hdr: off / auto / on) is live: the renderer asks at the end of every guest
// frame (SwitchWanted) and, when the output the setting and the display ask for is not the one the
// swap chain has, makes the swap chain again in the other format at that frame boundary
// (Renderer::SwitchHdr; HDR-PLAN.md). auto follows Windows' HDR switch for the display the window is
// on, also while the game runs and when the window moves to another display; on tries even when
// Windows says it is off. If the swap chain cannot be made HDR10 (no HDR10 colour space on the
// output, or a Vulkan surface without HDR10_ST2084) the SDR one stays, with one log line saying why,
// and nothing is asked again until the setting or what Windows says changes.
//
// The levels (paper white, peak) come from the settings or, at 0, from what
// Windows reports for the display the window is on: DXGI's IDXGIOutput6
// (GetDesc1: the colour space, the peak) and DisplayConfig (the SDR content
// brightness). The display is found from the window's HMONITOR over every
// adapter's outputs, not from the swap chain, so it is right when the GPU that
// renders is not the one the display hangs off (an iGPU rendering, the RTX
// driving the monitor). A thread reads it once a second while the setting is not off
// (or the output is HDR); a change of what it says (readable or not, HDR on or off) is believed
// when two reads in a row agree, so a display that flickers through a mode change does not rebuild
// the swap chain twice. A setting that leaves off reads it at once.
//
// Threading: Start/Stop on the lifecycle thread; Wanted/SetActive in between on
// it; Levels/Metadata, SwitchWanted and SwitchDone on the render thread; Active and Auto on any thread.
// ===========================================================================

#include <cstdint>
#include <string>

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
// With kHdrFlagExtendedCodes, for a source that holds HDR stage 3's float scene (not the 8-bit
// underlay): the table is read between its entries (linearly, and past the last one along its last
// step) instead of at the nearest of the 256, since the scene is not made of 8-bit values.
inline constexpr uint32_t kHdrFlagInterpolatedCodes = 1u << 3;

namespace hdr {

// Lifecycle thread, before the swap chain: reads the settings and, when it is not off, reads the
// display once; starts the thread that reads it again each second while it matters. `window` is
// the HWND (Windows); anywhere else no display is known.
void Start(plume::RenderWindow window);
// Lifecycle thread: stops the thread. Safe to call more than once.
void Stop();

// Lifecycle thread, after Start and before the swap chain: whether to try an HDR10 swap chain.
// Logs the reason when not.
bool Wanted();
// After the swap chain: whether it is HDR10. An HDR10 chain that was asked for and refused is not
// asked for again for this state of the setting and the display (SwitchWanted).
void SetActive(bool active);

// Render thread, at a frame boundary (no list open, nothing in flight is needed again): reads the
// setting (a change of it is taken here) and says whether the output the setting and the display
// ask for differs from `current_hdr`, the swap chain's. True with `*to_hdr` the format to switch to
// and `*why` (when given) what asked for it, in plain words for the log. False while the output is
// right, or while an earlier attempt to reach it was refused and neither the setting nor the
// display's state has changed since. One cvar read and a few atomic loads.
bool SwitchWanted(bool current_hdr, bool* to_hdr, std::string* why = nullptr);
// Render thread: how the attempt SwitchWanted proposed ended. `now_hdr` is what the swap chain is
// now (the old format when the attempt failed). Publishes Active() and, for a refusal
// (`wanted_hdr` != `now_hdr`), remembers it.
void SwitchDone(bool wanted_hdr, bool now_hdr);

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
