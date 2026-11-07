#pragma once
// ===========================================================================
// MCLA GPU renderer (Plume) -- what the app and the guest hooks call
// ===========================================================================
// src/gpu/ is the renderer the project is moving to: Plume over D3D12 today,
// Vulkan from the same code, one backend per build. Design, references and
// milestones:.
//
// The mode is mcla_gpu. It owns the window's presentation, so it excludes the
// SDK's D3D12Presenter and the legacy D3D12 runtime (mcla_native_gfx), and it
// needs the no-command-processor mode (mcla_native_gfx_nocp) for a process
// with no emulated GPU in it. Enabled() checks all of that once and says what
// is missing.
//
// Milestone M3 (this code): on top of M2's presentation (bindless heap, upload
// memory, host shaders, SDK overlays, idle presents, GPU timestamps, RenderDoc)
// -- every guest event that produces GPU work is captured with the device
// state it consumed and carried to the render thread in a byte stream (draws,
// clears, resolves, tiled rendering, the present), shader identities are exact,
// constants travel as versioned 64-byte blocks, and the render thread decodes
// and validates all of it against the shader pack (v4 since M4.1), the guest shader layout
// and the format tables..
//
// Milestone M4: the render thread draws it -- geometry
// (M4.4), the EDRAM targets with clears and tiling (M4.5), textures from guest
// memory, the resolve bridge and the guest's front buffer through the display
// gamma table (M4.6). Resolves written back into guest memory for the CPU, and
// memory read in another shape than it was resolved in, are M5.
//
// Call order (all on the UI thread except where noted):
//   CreateOverlayDrawer   from LarecompApp::OnCreateImmediateDrawer
//   SetShaderPackFolder   from LarecompApp::OnConfigurePaths, before Startup (optional)
//   SetShaderPackGate     from LarecompApp::SetupPresentation, before Startup (optional)
//   Startup               from LarecompApp::SetupPresentation, after the base
//   AttachShaderPacks     once the shader packs are built, when ShaderPacksAttached() is false
//   BeforeGuest*/AfterGuest*, OnGuestFrameEnd, OnGuestSwap
//                         guest threads, from src/native_gfx/hooks.cpp
//   RequestResize         window pixel-size changes
//   RequestTexture*       the pause menu's TEXTURE MODS rows (any thread)
//   PreviewRenderScale, GetRenderScaleStatus
//                         the pause menu's RENDER RES and FSR1 rows (any thread)
//   HdrOutputActive, HdrAuto*   the pause menu's HDR rows (any thread)
//   Shutdown              LarecompApp::FinishWindowClose (the window's close) and OnShutdown
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render_scale.h"

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace mcla::gpu {

// mcla_gpu is set and every prerequisite holds. Decided on the first call and
// fixed for the process (the cvars it reads all require a restart).
bool Enabled();

// The ImmediateDrawer the SDK draws its overlays through in this mode, or null
// when the mode is off. Needs no device: it records CPU-side frames for the
// renderer, which may not exist yet when the SDK asks for it.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();

// Creates the device, the swap chain on `window`, the renderer's components
// and the render thread, and connects the SDK's ImGui drawer (which must be
// using the drawer from CreateOverlayDrawer). False when any of it fails; the
// reason is logged and the app should not start.
bool Startup(rex::ui::Window* window, rex::ui::WindowedAppContext& app_context,
             rex::ui::ImGuiDrawer* imgui_drawer);

// --- the shader packs (shaders.h) ---------------------------------------------
// The renderer starts without the shader packs when there are none yet: on a first run they are
// built from the player's game files, which exist only after the ISO wizard, and the wizard is
// drawn by this renderer. A pack that is in place when Startup runs is attached by Startup, as
// always. Without one the renderer is pending: guest draws find no shaders and are skipped
// (counted; nothing draws guest content). Once a build finished, AttachShaderPacks makes the
// renderer usable, before the guest is launched.

// UI thread, before Startup (LarecompApp::OnConfigurePaths): the folder the packs are looked for
// in first and built into. Not called: <exe>/cache/shaders, else the local application data
// folder. The cvar shader_pack_dir replaces both and then is the only folder. A pack that an
// older install left in <exe>/assets is still read when the folder has none.
void SetShaderPackFolder(const std::filesystem::path& folder);
// UI thread, before Startup: lets the app decide whether Startup attaches the packs that are in
// place. Startup asks once, after the device exists; false leaves the renderer pending. The app
// does that for packs its stamp says are stale: they are rebuilt, and a pack the renderer has
// mapped could not be replaced. Not set: whatever pack is valid is attached, as before.
void SetShaderPackGate(std::function<bool()> gate);
// Where a build of the packs should write: shader_pack_dir, else the folder from
// SetShaderPackFolder, else the default (made, and tried with a real file). Any thread.
std::filesystem::path ShaderPackFolder();
// The folders the renderer looks for a pack in when it starts, in order (ShaderStore::SearchFolders
// of the folder SetShaderPackFolder gave). The first with a valid pack is the one the game uses.
// Any thread; needs no renderer (the pipeline prebuild, pipeline_prebuild.h, reads it).
std::vector<std::filesystem::path> ShaderPackSearchFolders();
// True once the main pack is attached (at Startup or by AttachShaderPacks); false with no
// renderer. Any thread. A build is needed when this is false after Startup.
bool ShaderPacksAttached();
// UI thread, after Startup and a finished build, before the guest runs: attaches the packs in
// `folder` (empty: ShaderPackFolder()) and starts the pipeline cache and precache against them.
// True when attached (also when they were already). False, logged, when the pack is missing or
// invalid there: the renderer stays pending.
bool AttachShaderPacks(const std::filesystem::path& folder);

// --- guest capture (hooks.cpp) ------------------------------------------------
// Each hooked D3D call is bracketed: Before* runs before the original (it reads
// what the original consumes and clears -- the constant dirty masks, the state a
// clear or resolve reads), After* after it (the state the original set up). The
// token carries what Before* saw; an inactive token makes After* a no-op, so a
// call bracketed across startup or shutdown is simply not captured.

struct GuestCallToken {
  uint64_t vs_dirty = 0;  // D3DDevice +0 before the call
  uint64_t ps_dirty = 0;  // +8
  uint32_t snapshot = 0;  // event pre-state slot (1-based), 0 = none
  bool active = false;
};

enum class GuestEvent : uint8_t { kClear, kResolve, kTilingBegin, kTilingEnd };

// D3DDevice_DrawIndexedVertices / DrawVertices / BeginVertices.
GuestCallToken BeforeGuestDraw(const uint8_t* base, uint32_t device);
void AfterGuestDrawIndexed(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                           uint32_t count);
void AfterGuestDrawVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                            uint32_t primitive, uint32_t start_vertex, uint32_t count);
// `buffer` is BeginVertices' return value: where the caller writes the vertices.
void AfterGuestBeginVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                             uint32_t primitive, uint32_t count, uint32_t stride,
                             uint32_t buffer);
void AfterGuestEndVertices(const uint8_t* base, uint32_t device);

// ClearF, Resolve, BeginTiling, EndTiling. `stack_pointer` is the caller's r1
// (Resolve reads ClearStencil and pParameters from the stack); 0 elsewhere.
GuestCallToken BeforeGuestEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                                uint32_t stack_pointer);
// `caller` is the return address (LR) the hook was entered with.
void AfterGuestClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                     uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil);
void AfterGuestResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                       uint32_t caller, uint32_t flags, uint32_t source_rect,
                       uint32_t dest_texture, uint32_t dest_point, uint32_t dest_level,
                       uint32_t dest_slice, uint32_t clear_color, float clear_z);
void AfterGuestBeginTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t caller, uint32_t flags, uint32_t tile_count, uint32_t rects,
                           uint32_t clear_color, float clear_z, uint32_t clear_stencil);
void AfterGuestEndTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                         uint32_t caller, uint32_t flags, uint32_t rects, uint32_t dest_texture,
                         uint32_t clear_color, float clear_z, uint32_t clear_stencil,
                         uint32_t params);

// rage::grcDevice::EndFrame, after the original. Keeps the game off its
// command-buffer cache: without a replay of our own, every draw has
// to reach the hooks. Applies whenever the mode is enabled.
void OnGuestFrameEnd(uint8_t* base);

// Guest render thread, D3DDevice_Swap, after the original (`device` and
// `front_buffer` are its r3/r4, read before it). Queues the present behind the
// frame's records and waits until the render thread ran it. No-op unless
// Startup succeeded and Shutdown has not begun.
void OnGuestSwap(const uint8_t* base, uint32_t device, uint32_t front_buffer);

// --- guest GPU fences ------------------------------------------
// The render thread plays the guest's GPU: it holds the write of the guest's
// retired fence from Startup to Shutdown and publishes each fence when it has
// executed every record captured before it.

// sub_82411CD0, AFTER the original. `value` is the device's issued counter
// read before it -- what the inserted EVENT_WRITE carries. Becomes a fence
// record when the capture is on.
void OnGuestFenceInserted(uint32_t device, uint32_t value);
// True while the render thread writes the retired fence and the capture is on.
// D3DDevice_BlockUntilIdle then waits on a real fence.
bool RetiresGuestFences();
// sub_82425C18, the guest's "GPU is hung" detector, in this mode: logs and
// returns; the hook makes the guest keep waiting. Any guest thread.
void OnGuestGpuHangCheck(const uint8_t* base, uint32_t device);
// A D3DDevice_BlockOnFence call that found its fence pending, and how long it
// waited. Any guest thread.
void NoteGuestFenceWait(uint64_t wait_ns);

// --- CPU access to resolve destinations (M5.1) ---------------------

// D3DResource_Lock (sub_82421CA0), BEFORE the original: r3 the resource, r6
// the level, r9 the level's address (with its window), r10 its size. A range
// that holds the destination of a resolve the capture recorded becomes a lock
// record. Any guest thread.
void OnGuestLock(uint32_t resource, uint32_t level, uint32_t address, uint32_t size);

// --- the scene's composition (M6c) ---------------------------------

// sub_8260EFD0 (the end of the post chain, inside the post object's bucket-28
// draw), AFTER the original: the scene's image is composed, nothing has copied
// it yet, and what the frame draws after it is screen-space. `bucket`: the
// bucket's bit. Becomes a composite record when the capture is on. Guest
// render thread.
void OnGuestSceneComposited(uint32_t bucket);

// --- occlusion queries (occlusion.h) --------------------------------------

// sub_824302F0 (D3DQuery::Issue's sample count event: `query` the D3DQuery object,
// `is_begin` its begin or end), BEFORE the original. Guest render thread.
void OnGuestOcclusionEvent(const uint8_t* base, uint32_t query, bool is_begin);
// sub_8241E088 (D3DQuery::GetData), BEFORE the original: a count the host has delivered
// for the query is written to its record first. Guest thread.
void OnGuestQueryGetData(const uint8_t* base, uint32_t query);

// UI thread, on a window pixel-size change. The rebuild happens on the render
// thread at the next present (an idle one if the guest is not swapping).
void RequestResize();

// Any thread. Texture mods: every texture the renderer holds to the dump
// folder (F2 does only the next frame's), and the replacement folders read
// again (textures.h). Nothing when the Plume renderer is not running.
void RequestTextureDumpAll();
void RequestTextureReload();

// Any thread: the internal resolution scale these settings choose
// (render_scale.h) for the window as it is now -- the swap chain's
// last size; before the renderer is up, a window measured as the game's
// screen. The pause menu labels its RENDER RES and FSR1 rows with it.
RenderScaleChoice PreviewRenderScale(double asked, bool fsr, FsrPreset preset);
// Render thread (the temporal system, at every frame boundary): the scale the settings ask for now,
// the cvars read and the window as it is (the swap chain's last size; 0x0 in the choice's window
// fields until the renderer has published one). The renderer follows it while the game runs.
RenderScaleChoice WantedRenderScale();
// Any thread: the scale the renderer's images of the screen are made at now, and the one the settings
// ask for that could not be made for want of video memory (render_scale.h RenderScaleStatus). The
// pause menu's RENDER RES row says so beside the value.
RenderScaleStatus GetRenderScaleStatus();

// Any thread: the window's swap chain is HDR10 (hdr.h). False before the renderer is up and when
// HDR is off or could not be had; the pause menu greys its HDR level rows then.
bool HdrOutputActive();
// Any thread: what 0 (AUTO) stands for in the HDR level settings now, in nits: Windows' report for
// the display the window is on, else 200 and 1000.
float HdrAutoPaperWhiteNits();
float HdrAutoPeakNits();

// UI thread, at app shutdown: stops the capture, disconnects the overlay passes,
// stops the render thread (with a deadline), drains the GPU and releases every
// GPU object, device last, then hands the retired fence back to the vblank
// thread. Safe to call more than once.
void Shutdown();

// graphics_backend = "auto" as the Plume renderer decides it (ChooseAutoBackend in backend.h):
// true when that is Vulkan. For the emulated GPU's backend (larecomp_app.h), which follows the same
// setting; backend.h itself needs Plume's headers, this one doesn't.
// With `adapter` given, and Vulkan chosen, it receives the name of the GPU that choice was made for.
bool AutoBackendIsVulkan(const std::string& preferred_adapter, std::string* adapter = nullptr);

// The emulated GPU (Xenia) path, which has no say in which GPU it runs on: the SDK takes Direct3D 12's
// first adapter that is not software and Vulkan's first device, and on a PC with two GPUs that is often
// the weaker one, the one that drives the screen. This is the number to give the SDK's d3d12_adapter
// (`vulkan` false: the adapter in IDXGIFactory1's order) or vulkan_device (the device in the loader's
// order) so that it runs on the GPU the Plume renderer would use: `preferred_adapter` (mcla_gpu_adapter,
// or the GPU graphics_backend = auto chose Vulkan for) by name; else on D3D12 the adapter Windows'
// graphics setting for this exe names, else the high-performance GPU; else on Vulkan a discrete GPU
// before an integrated one. -1: no opinion, the SDK chooses (also on Linux for D3D12, which has none).
int XeniaAdapterIndex(bool vulkan, const std::string& preferred_adapter);

// --- the temporal upscalers (temporal_backend.h) ---------------------------------------------------
// What the player's Upscaling rows need, in plain types (the rest of the app sees no Plume header).
// mcla_gpu_upscaler is off | fsr31 | fsr4 | dlss; a choice this PC cannot run is treated as off (DLSS
// that can run only from the next start leaves the running path alone: `kept` below) and
// the menu does not list it (DLSS without its files is listed, greyed, as "needs setup").
enum class Upscaler : uint8_t { kOff, kFsr31, kFsr4, kDlss };  // stored in 2 bits in places
const char* UpscalerName(Upscaler upscaler);  // OFF, FSR 3.1, FSR 4, DLSS

// The vendors' quality modes: their names and the render size as a share of the output. FSR 3.1
// and FSR 4 share one table (Native AA 100%, Quality 67%, Balanced 59%, Performance 50%, Ultra
// Performance 33%); DLSS's differs in the names (DLAA for the first) and in Balanced (58%).
enum class UpscaleMode : uint8_t { kNativeAa, kQuality, kBalanced, kPerformance, kUltraPerformance,
                                   kCount };
struct ModeInfo {
  const char* cvar;   // native_aa (dlaa for DLSS), quality, balanced, performance, ultra_performance
  const char* name;   // NATIVE AA (DLAA), QUALITY, ...
  uint32_t percent;   // 100, 67, 59 (DLSS 58), 50, 33
  double ratio;       // output size over render size: 1, 1.5, 1.7 (DLSS 1.724), 2, 3
};
// `upscaler` picks the table (FSR's for off, FSR 3.1 and FSR 4).
const ModeInfo& ModeInfoOf(Upscaler upscaler, UpscaleMode mode);
inline const ModeInfo& ModeInfoOf(UpscaleMode mode) { return ModeInfoOf(Upscaler::kFsr31, mode); }

// Any thread. Written by the renderer's upscaler backends when their probes end (and when
// something fails) and by the temporal system as frames go.
struct UpscalerAvailability {
  bool probed = false;  // the FidelityFX answers (fsr31, fsr4) are final
  bool fsr31 = false;   // can run on this PC: listed under Upscaling
  bool fsr4 = false;    // its provider really loads here
  // NVIDIA DLSS (through Streamline). Three states of a card that could run it: the files are
  // there and it can run (dlss); the files are missing or fail their signature check (needs setup:
  // listed greyed, the game runs); it can run, but only from the next start (Vulkan: the device is
  // made with Streamline's help, so the choice has to be there when the game starts).
  bool dlss_probed = false;         // the DLSS answers are final
  bool dlss = false;                // can run on this PC now: listed under Upscaling
  bool dlss_needs_setup = false;    // an NVIDIA RTX card, files missing: listed greyed
  bool dlss_after_restart = false;  // listed; runs from the next start
  // DLSS takes a render size of its own for each mode at an output size (Ultra Performance's is exact,
  // the others lie in a range), so a mode can be unable to run at the output the renderer works at:
  // Ultra Performance needs a scene of a third of the output, which is under the 0.5x the scene is
  // drawn at as its smallest for an output under 1.5x, and off the scale grid at 2560x1440. Bit i
  // set: UpscaleMode i cannot run there, as the DLSS runtime said (temporal_backend.h ModeVerdicts).
  // No bit while that is not known yet (the runtime has not started) or DLSS cannot run at all. The
  // pause menu's DLSS mode row says "(N/A)" for these, and a pick of one keeps the mode that runs.
  uint32_t dlss_modes_refused = 0;
  bool DlssModeRefused(UpscaleMode mode) const {
    return uint32_t(mode) < uint32_t(UpscaleMode::kCount) &&
           ((dlss_modes_refused >> uint32_t(mode)) & 1u) != 0;
  }
  Upscaler running = Upscaler::kOff;  // the one doing the work this moment
  // DLSS is chosen but this game can run it only from the next start (dlss_after_restart): the
  // upscaler the renderer keeps until then, with the mode it had (kOff: none runs, or nothing is
  // waiting). The pause menu shows that one as the active one, greyed, instead of the choice.
  Upscaler kept = Upscaler::kOff;
  // The settings ask for another upscaler or mode than the one running and its context is being made
  // (a live change, temporal.h): the old one goes on until it is ready.
  bool switching = false;
};
UpscalerAvailability GetUpscalerAvailability();

// --- MSAA --------------------------------------------------------------------------------------
// The native renderer never turns the game's own MSAA switch off (hooks/graphics.cpp MsaaOff): the
// game always draws its 2 sample layout, and the host's MSAA level (EdramTargets::msaa_level: 1, 2,
// 4 or 8 samples on the images of the screen) is the player's choice, 1 for OFF and while an upscaler
// runs (an upscaler smooths the picture itself, and MSAA under it only costs time). The level
// changes while the game runs (msaa_level.h).
// The host MSAA level changes while the game runs (msaa_level.h): true from when a new level has
// stood still until its pipelines are made and the targets are made again. The pause menu's MSAA
// row says (WAIT). Any thread.
bool MsaaSwitching();

// --- the upscaler as the start found it (the size of the 3D scene) -------------------------------
// The scene is drawn smaller than the output only while an upscaler can make it whole again. The
// scene's first targets are made for the choice read once at the start: the upscaler and mode
// chosen, and whether this PC can run that upscaler (known when the upscaler probe has ended).
// `ratio` is the mode's output over render, per axis (1 when no upscaler runs); the renderer turns
// it into the scene's scale (render_scale.h ChooseSceneScale). The host's MSAA level is decided from
// it too (EdramTargets::DecideScene: 1 when `runs`). A change of upscaler or mode afterwards is made
// live by the temporal system at a frame boundary (temporal.h, "Changing the path"), and the MSAA
// level follows it (msaa_level.h).
struct StartUpscale {
  bool decided = false;
  bool probed = false;  // the upscaler probe had ended when it was decided
  Upscaler upscaler = Upscaler::kOff;  // as chosen
  bool runs = false;                   // chosen, and this PC can run it
  UpscaleMode mode = UpscaleMode::kNativeAa;
  double ratio = 1.0;  // the mode's, when `runs`; else 1
};
// Decides on the first call (any thread; waits a few seconds for the probe when an upscaler is
// chosen), the same answer ever after. The game's own thread calls it first, when it makes its
// renderer (hooks/graphics.cpp MsaaOff), so the wait for the probe is not the renderer's.
StartUpscale GetStartUpscale();
// What has been decided so far, never waiting: `decided` is false before.
StartUpscale PeekStartUpscale();

// --- frame generation (frame_generation.h) -----------------------------------------------------------
// AMD's FSR 3.1 frame generation, mcla_gpu_frame_generation = off | fsr31 (live). Any thread.
struct FrameGenerationAvailability {
  bool probed = false;     // the runtime's answer is final
  bool available = false;  // FSR 3.1 frame generation can run on this PC: the pause menu lists the row
  bool failed = false;     // it was turned off for the rest of the session after a failure
  // The card has no room for it now: frame generation (its images and AMD's context, hundreds of MB
  // at 4K) would take video memory past the line where the budget starts releasing the caches, or
  // AMD's runtime ran out of it making the context. It is off meanwhile, looked at again every few
  // seconds, and `why` says how much it needs and how much is in use. Not a failure: the row stays.
  bool no_room = false;
  bool running = false;    // on: the swap chain is on the pacer's queue (the cap and the reducer follow)
  uint32_t refresh_hz = 0; // the display the window is on (its highest mode), 0 until read
  // HALF DISPLAY's cap in rendered frames a second (the floor of 97% of half the refresh rate), 0 where
  // it is not offered (a display under 100 Hz, or an unknown one): the player's own cap applies there.
  uint32_t half_display = 0;
  std::string why;         // not available, failed or no room: one plain sentence for the log and the launcher
};
FrameGenerationAvailability GetFrameGenerationAvailability();
// Whether frame generation runs now (the swap chain is on the pacer's queue).
bool FrameGenerationRunning();
// The frame limiter's cap now: `player_limit` (fps_limit) normally, the frame generation cap
// (mcla_gpu_frame_generation_fps_limit) while it runs. 0 is uncapped. Counts rendered frames.
int32_t FrameGenerationFpsLimit(int32_t player_limit);

// --- DLSS frame generation (dlss_frame_generation.h) -------------------------------------------------
// NVIDIA's DLSS frame generation through Streamline, mcla_gpu_frame_generation = dlss (live), with
// mcla_gpu_frame_generation_multiplier beside it. Any thread.
struct DlssFrameGenerationAvailability {
  bool probed = false;         // the answer is final (Streamline has started and been asked)
  bool listed = false;         // the FRAME GENERATION row lists DLSS: an RTX 40 or 50 series card with the files
  bool supported = false;      // Streamline said it can run here now
  bool after_restart = false;  // listed, but runs from the next start (Vulkan: the device is made through
                               // Streamline only when it is saved at the start)
  bool failed = false;         // it was turned off for the rest of the session after a failure
  bool running = false;        // on: the swap chain is Streamline's proxy chain
  uint32_t max_multiplier = 2; // 2 to 4: the most the card takes (2 until the plugin has said)
  bool multiplier_choice = false;  // more than 2x is possible: the pause menu has the multiplier row
  uint32_t refresh_hz = 0;
  std::string why;             // not supported, or failed: one plain sentence for the log and the launcher
};
DlssFrameGenerationAvailability GetDlssFrameGenerationAvailability();
// Whether DLSS frame generation runs now (the frame cap and the latency reducer follow).
bool DlssFrameGenerationRunning();
// The multiplier the setting comes to on this card: 2, 3 or 4.
uint32_t DlssFrameGenerationMultiplier();
// The frame limiter's cap now: `player_limit` normally, the frame generation cap
// (mcla_gpu_frame_generation_fps_limit; HALF DISPLAY is the display's rate over the multiplier) while it
// runs. 0 is uncapped. Counts rendered frames.
int32_t DlssFrameGenerationFpsLimit(int32_t player_limit);

}  // namespace mcla::gpu
