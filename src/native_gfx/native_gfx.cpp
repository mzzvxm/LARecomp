#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — entry point, cvar and guest swap hook.

// The diagnostic loggers in this runtime use fopen/fprintf; MSVC deprecates the
// former in favour of fopen_s and the warning is noise here.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "native_gfx.h"

#include <atomic>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/keybinds.h>

#include <rex/graphics/native_guest_renderer.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

#include "d3d12/blit_pass.h"
#include "d3d12/presenter_output.h"
#include "d3d12/d3d12_smoke_triangle.h"
#include "d3d12/context.h"
#include "d3d12/first_draw.h"
#include "d3d12/frame_capture.h"
#include "d3d12/memory_census.h"
#include "guest/render_state.h"
#include "guest/texture_format.h"
#include "d3d12/pipeline_cache.h"
#include "d3d12/render_target_pool.h"
#include "d3d12/renderdoc_hook.h"
#include "d3d12/resource_cache.h"
#include "d3d12/shader_db.h"
#include "d3d12/texture_binding.h"
#include "d3d12/texture_cache.h"

REXCVAR_DEFINE_BOOL(mcla_native_gfx, false, "MCLA/NativeGfx",
                    "MCLA Native Graphics Runtime. OFF (default): the game renders through the "
                    "normal Xenia-based path, nothing changes. ON: the native D3D12 backend is "
                    "initialized and the bring-up instrumentation runs (see "
                    "mcla_native_gfx_present for the presentation takeover).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_firstdraw, false, "MCLA/NativeGfx",
                    "Execute ONE real MCLA draw through the native pipeline, into a dedicated "
                    "render target, and write mcla_native_gfx_firstdraw.txt/.tga. Does not "
                    "present and does not touch the guest render path, so it cannot fight the "
                    "Xenia command processor. Runs once per session.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_capture, 0, "MCLA/NativeGfx",
                      "Accumulate this many guest draws of the main scene pass into one native "
                      "render target and write mcla_native_gfx_frame.txt/.tga. 0 disables. "
                      "Supersedes mcla_native_gfx_firstdraw when non-zero.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_capture_delay, 0, "MCLA/NativeGfx",
                      "Frame boundaries to skip before arming the one-shot capture. The early "
                      "frames after boot show low-detail placeholder textures (LZX streaming has "
                      "not caught up) and an unconverged HDR exposure, giving a dark, muddy "
                      "image. Set e.g. 300 to capture a fully-streamed, well-exposed frame.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_auxstage, 0, "MCLA/NativeGfx",
                      "Diagnostic: how far to take auxiliary (non-anchor) passes. "
                      "0 = not rendered at all. 1 = acquire the pooled target only. "
                      "2 = also bind it and clear. 3 = also record the draw. "
                      "A single auxiliary draw hangs the GPU, so this bisects which "
                      "stage does it: resource creation, binding, or the draw itself.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_dumprt, false, "MCLA/NativeGfx",
                    "Diagnostic: write every large resolve destination to a .tga at the end "
                    "of the capture. Counting a fetch as resolved only proves a resource was "
                    "handed back, not that it holds the right pixels. Costs a full GPU stall "
                    "per target, so it is off by default.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_present, false, "MCLA/NativeGfx",
                    "Native presentation takeover (smoke-test stage): suppress the guest swap "
                    "and present the native frame instead. MUST stay off while the Xenia command "
                    "processor is still consuming the game's draws: without swaps its D3D12 "
                    "submission never closes and the GPU eventually TDRs (observed DEVICE_HUNG). "
                    "It becomes the default path once the native runtime executes the draws "
                    "itself and the guest command stream is empty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_continuous, false, "MCLA/NativeGfx",
                    "EXPERIMENTAL gameplay mode: render every guest frame through the native "
                    "pipeline and present it, instead of the one-shot capture. Keeps all caches "
                    "across frames, so the HDR exposure adaptation converges. Does NOT suppress "
                    "the guest swap (avoids the command-processor TDR), so the guest still runs "
                    "underneath. Requires mcla_native_gfx.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_texture_swizzle, false, "MCLA/NativeGfx",
                    "Apply the fetch constant's 12-bit swizzle to the SRV's "
                    "Shader4ComponentMapping instead of the D3D12 default. The two encodings "
                    "match one-for-one (0..3 component, 4 = zero, 5 = one), and 34 of MCLA's "
                    "k_8_8_8_8 textures -- the colour-grading LUTs bound on every draw -- carry "
                    "0x60A = (Z,Y,X,W), so in principle the default mapping swaps red and blue. "
                    "In practice turning it on tints the sky dome red, which means the untiler's "
                    "8-in-32 endian swap already puts the channels in host order for at least "
                    "some of them and the swizzle is then applied twice. OFF until a per-format "
                    "host swizzle (the piece Xenia composes with the guest one) is worked out.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_alpha_ref, true, "MCLA/NativeGfx",
                    "Feed SharedConstants.g_AlphaThreshold from RB_ALPHA_REF and RB_COLORCONTROL "
                    "instead of the fixed 0.5 the bring-up used. The ALPHA lines the diagnostic "
                    "asked for came back, and they settle it: every (func, ref) pair MCLA uses is "
                    "func=4 (GREATER) with ref in {0, 0.003922, 0.007843, 0.015686} plus one "
                    "func=5 (NOTEQUAL) ref=0. The reference is never anywhere near 0.5, so the "
                    "bring-up constant discarded every fragment whose alpha fell between the real "
                    "reference and 0.5 -- which is the entire car body: measured in \"car.rdc\", "
                    "the paint draws (26615/26653) upload g_AlphaThreshold=0.5 and RenderDoc "
                    "reports shaderDiscarded over the whole body, while the same car's full "
                    "silhouette is present in the depth buffer. Debugging the discarded pixel at "
                    "(700, 400) of draw 26653 gives the number outright: the shader computes "
                    "alpha = 0.04375, subtracts the 0.5 threshold and discards on the negative "
                    "result -- against the real reference (<= 0.015686) that fragment is kept. "
                    "Wheels and glass survived because "
                    "their alpha is 1 or the test is off, which is exactly the 'only rims and "
                    "windows render' symptom. The earlier counter-evidence (palm fronds turning "
                    "into solid dark quads with this ON) was measured BEFORE the texcoord swap "
                    "fix, i.e. with transposed UVs, so the foliage was sampling the wrong texels "
                    "and its alpha mask meant nothing -- it has to be re-measured. ON by default "
                    "now; the switch stays so the fixed 0.5 can be put back while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_half_pixel, true, "MCLA/NativeGfx",
                    "Fill SharedConstants.g_HalfPixelOffset with (-1/viewport_w, +1/viewport_h). "
                    "23 of the 25 vertex shaders in a captured frame end with "
                    "SV_Position.xy += g_HalfPixelOffset.xy * w, the D3D9 -> D3D10+ pixel-centre "
                    "correction; left at zero every draw lands half a pixel off the texel grid, "
                    "which is where the soft 2D edges come from. On by default because the "
                    "shaders demand it; the switch exists so it can be taken out of the picture "
                    "while bisecting a colour or coverage regression.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_swapped_texcoords, true, "MCLA/NativeGfx",
                    "Fill SharedConstants.g_SwappedTexcoords from the vertex declaration. A RAGE "
                    "vertex packs a 32-bit POSITION and a 16-bit TEXCOORD in one stream, but a "
                    "stream can only be byte-swapped at one width; swapped as 8in32 the 16-bit "
                    "pair arrives as (v, u) and every texture renders transposed, which looks "
                    "like a 90-degree rotation to the left. The translated shaders already carry "
                    "the fix (tfetchTexcoord -> value.yxwz) and only needed the mask. On by "
                    "default; the switch exists to take it back out while bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_texcache_mb, 768, "MCLA/NativeGfx",
                      "Byte budget (MiB) for the native texture cache. The cache is keyed by the "
                      "fetch constant, so a streaming game keeps producing new keys as it pages "
                      "geometry and textures in and out; without a budget every one of them stays "
                      "resident for the session and the footprint grows without bound. Over the "
                      "budget, least-recently-used entries are released (fence-gated, never one "
                      "the current frame bound). 0 = unbounded, the old behaviour.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_census, 120, "MCLA/NativeGfx",
                      "Frame boundaries between memory census reports written to "
                      "native_gfx_mem.txt: process commit/working set, DXGI local (VRAM) and "
                      "non-local usage, and the live size of every native cache. The pool whose "
                      "delta tracks the process delta is the leak. 0 disables.");

namespace mcla::native_gfx {

namespace {

enum class InitState { kNotStarted, kActive, kFailed };
std::atomic<InitState> g_state{InitState::kNotStarted};
// Set by the keybind (UI thread), consumed at the frame boundary (guest thread).
std::atomic<bool> g_rdc_capture_request{false};
D3D12SmokeTriangle g_triangle;
rex::ui::Presenter* g_presenter = nullptr;

// The real-draw subsystems. Created lazily on the first attempt so a session
// that never enables the first-draw path pays nothing.
D3D12Context g_draw_context;
ShaderDatabase g_draw_shaders;
BufferCache g_buffers;
TextureCache g_textures;
TextureBinder g_binder;
PipelineCache g_pipelines;
RenderTargetPool g_render_targets;
bool g_draw_ready = false;
bool g_draw_init_failed = false;
const rex::ui::d3d12::D3D12Provider* g_provider = nullptr;

// Continuous-mode present path: a presenter output and a fullscreen blit on the
// SAME device as g_draw_context (both come from the shared provider), so the
// pooled display target can be sampled straight into the guest output.
PresenterOutput g_present_output;
BlitPass g_blit;
bool g_present_ready = false;

// One-time lazy setup: resolve the SDK graphics system, require the D3D12
// backend, initialize the smoke-test renderer. Any failure latches kFailed
// and logs once; the guest swap then proceeds normally.
bool TryInitialize() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    REXLOG_ERROR("[native_gfx] no Runtime instance");
    return false;
  }
  // The concrete GraphicsSystem lives inside the rexgpu-xenos plugin, which
  // consumers never link; provider() and presenter() are on the interface.
  auto* graphics = runtime->graphics_system();
  if (!graphics || !graphics->has_presentation()) {
    REXLOG_ERROR("[native_gfx] graphics system has no presentation, cannot attach");
    return false;
  }
  const std::string backend = REXCVAR_QUERY(std::string, graphics_backend);
  if (backend != "auto" && backend != "d3d12") {
    REXLOG_ERROR("[native_gfx] graphics_backend is '{}', native runtime needs d3d12", backend);
    return false;
  }
  auto* provider = static_cast<rex::ui::d3d12::D3D12Provider*>(graphics->provider());
  if (!provider) {
    REXLOG_ERROR("[native_gfx] no graphics provider");
    return false;
  }
  g_provider = provider;
  if (!g_triangle.Initialize(*provider)) {
    return false;
  }
  g_presenter = graphics->presenter();
  if (g_presenter == nullptr) {
    return false;
  }
  // Take F11 over from the SDK's bind. The SDK one brackets the capture around
  // the EMULATED command processor's swap, which is the wrong window once this
  // runtime owns the frame: our whole batch is recorded and submitted between
  // frame boundaries, so a capture opened at the guest swap contains the
  // presenter blit and nothing else. Registering a second bind on the same key
  // would never fire -- ProcessKeyEvent returns on the first match and the SDK
  // registered first -- so the SDK's is unregistered (which only nulls its
  // callback) and ours is registered after it.
  //
  // Only done once this runtime is actually active; when it is off the SDK bind
  // stays as it was.
  rex::ui::UnregisterBind("bind_renderdoc_capture");
  rex::ui::RegisterBind("bind_native_gfx_renderdoc", "F11",
                        "Capture the next guest frame with RenderDoc (native runtime)",
                        [] { RequestRenderDocCapture(); });
  return true;
}

}  // namespace

bool Active() {
  if (!REXCVAR_GET(mcla_native_gfx)) {
    return false;
  }
  InitState state = g_state.load(std::memory_order_acquire);
  if (state == InitState::kActive) {
    return true;
  }
  if (state == InitState::kFailed) {
    return false;
  }
  const bool ok = TryInitialize();
  g_state.store(ok ? InitState::kActive : InitState::kFailed, std::memory_order_release);
  if (!ok) {
    REXLOG_ERROR("[native_gfx] initialization failed; staying on the normal path");
  }
  return ok;
}

// Registered with the command processor via SetNativeGuestOutputRenderer. Runs
// on the CP thread at swap. Blits the display target the guest thread published
// (PrepareContinuousDisplay) into the guest output through the SDK's external
// blit, which records on the CP's own command list. Returns true when it served
// the frame -- which is what keeps the emulated-draw suppression engaged.
bool NativeGuestOutputCallback(const rex::graphics::NativeGuestOutputRenderContext& ctx,
                               void* /*user*/) {
  uint32_t fmt = 0, w = 0, h = 0;
  auto* display = static_cast<ID3D12Resource*>(GetContinuousDisplayResource(&fmt, &w, &h));
  static int cb = 0;
  if (cb < 4) {
    REXLOG_INFO("[native_gfx] callback#{} display={}", cb++, (void*)display);
  }
  // TEMP DIAG (remove after): ground-truth whether the swap callback fires and
  // whether the guest thread ever published a display, since the logger is blind
  // on non-TTY stdout.
  {
    static unsigned total = 0, null_cnt = 0, ok_cnt = 0;
    ++total;
    if (display) ++ok_cnt; else ++null_cnt;
    if (total <= 10u || (total % 60u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "callback total=%u display_ok=%u display_null=%u last=%ux%u fmt=%u\n",
                     total, ok_cnt, null_cnt, w, h, fmt);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // Always claim the frame once continuous mode is on. Returning false here
  // falls through to the emulated gamma/FXAA blit -- but the command processor
  // already wrapped the guest output for us (NativeRhiBeginFrame), and that
  // emulated path then runs against a guest output in the wrong state and
  // faults. When there is no display yet (the first frames, before the guest
  // thread has published one) we simply present the untouched output.
  if (display) {
    // Through the RHI device: the blit implementation lives in the rexgpu-xenos
    // plugin, which this executable never links.
    ctx.device->BlitExternalToGuestOutput(ctx.guest_output, display, fmt, ctx.guest_output_width,
                                          ctx.guest_output_height);
  }
  return true;
}

void TryFirstDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                  uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                  bool indexed) {
  const bool want_continuous = REXCVAR_GET(mcla_native_gfx_continuous);
  const bool want_capture = REXCVAR_GET(mcla_native_gfx_capture) > 0 || want_continuous;
  const bool want_first = REXCVAR_GET(mcla_native_gfx_firstdraw);
  if ((!want_first && !want_capture) || g_draw_init_failed) {
    return;
  }
  // Continuous mode never "finishes"; the one-shot done-check only applies to
  // the bounded capture.
  if (!want_continuous && (want_capture ? FrameCaptureDone() : FirstRealDrawDone())) {
    return;
  }
  if (!g_draw_ready) {
    // Report which subsystem failed: collapsing these into one condition
    // makes an initialization failure impossible to diagnose from the log.
    const char* failed = nullptr;
    if (!g_provider) {
      failed = "no D3D12 provider";
    } else if (!g_draw_context.Initialize(*g_provider)) {
      failed = "D3D12Context";
    } else if (!g_pipelines.Initialize(g_draw_context)) {
      failed = "PipelineCache (root signature)";
    } else if (!g_binder.Initialize(g_draw_context)) {
      failed = "TextureBinder (descriptor heaps)";
    } else if (!g_draw_shaders.Load()) {
      failed = "ShaderDatabase (assets/mcla_shaders.pack)";
    }
    // The bridge that lets a texture fetch find the render target that
    // produced it, instead of decoding never-written guest memory.
    g_textures.SetRenderTargetLookup(&g_render_targets);
    // Guest writes to an uploaded vertex/index range have to invalidate it.
    // Without this a range was uploaded once and never again, so a mesh
    // streamed into recycled memory rendered with the previous mesh's bytes.
    // A failure here is not fatal: it only restores that old behaviour.
    g_buffers.StartWatchingGuestWrites();
    // Same hole on the texture side: a tile sampled while it was still
    // streaming in stayed half-decoded forever, which is the grid of black
    // squares on the map screen.
    g_textures.StartWatchingGuestWrites();
    if (failed) {
      REXLOG_ERROR("[native_gfx] first-draw initialization failed at: {}", failed);
      g_draw_init_failed = true;
      return;
    }
    g_draw_ready = true;

    // Continuous mode registers a native guest-output renderer with the
    // command processor (Skate 3 model). At swap, the CP calls our callback to
    // paint the guest output, and suppresses the emulated draws we replace
    // (native_render_suppress_* cvars). One producer, in the CP's own frame --
    // no separate present, no race, no TDR.
    if (REXCVAR_GET(mcla_native_gfx_continuous)) {
      rex::graphics::SetNativeGuestOutputRenderer(&NativeGuestOutputCallback, nullptr);
      g_present_ready = true;
      SetContinuousMode(true);
      REXLOG_INFO("[native_gfx] continuous mode active (native guest-output renderer registered)");
    }
  }
  // Continuous mode has no draw limit (the finish-on-limit path is gated off in
  // frame_capture); it just needs a non-zero value so the draw is not skipped.
  const uint32_t capture_limit =
      want_continuous ? 1000000u : uint32_t(REXCVAR_GET(mcla_native_gfx_capture));
  // Auxiliary passes render fully in continuous mode (aux_stage 3) by default,
  // BUT an explicit mcla_native_gfx_auxstage override is honoured even in
  // continuous mode. This matters because a single auxiliary-pass draw can hang
  // the GPU (DEVICE_HUNG TDR, confirmed by DRED: an auxiliary DrawIndexedInstanced
  // never completes). Setting auxstage=2 (bind+clear, no aux draw) lets that be
  // bisected — and used as a stopgap — without leaving continuous mode. 0 is
  // treated as "use the default 3" so the untouched default keeps full passes.
  const uint32_t aux_override = uint32_t(REXCVAR_GET(mcla_native_gfx_auxstage));
  const uint32_t aux_stage = want_continuous ? (aux_override ? aux_override : 3u) : aux_override;
  if (capture_limit) {
    // Multi-draw capture supersedes the single-draw diagnostic: both write to
    // the same subsystems, and running them together would interleave two
    // unrelated render targets in one command list.
    CaptureDraw(base, dev, primitive_type, element_count, start_element, base_vertex, indexed,
                capture_limit, g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder,
                g_pipelines, g_render_targets, aux_stage);
    return;
  }
  TryFirstRealDraw(base, dev, primitive_type, element_count, start_element, base_vertex, indexed,
                   g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder, g_pipelines);
}


void NotifyResolve(const uint8_t* base, uint32_t dev, uint32_t flags, uint32_t dest_texture,
                   uint32_t source_rect, uint32_t dest_point) {
  if (!Active() || !dest_texture) {
    return;
  }
  // The destination is a D3D texture object holding the same six-dword fetch
  // constant a later draw samples, at +28. Confirmed against the guest's own
  // resolve (sub_82420BA8), which reads the address as
  // *(dest+32) & 0xFFFFF000 and the pitch as (*(dest+28) >> 17) & 0x3FE0 —
  // bit for bit what DecodeTextureFetch computes from d[1] and d[0].
  constexpr uint32_t kDestFetchOffset = 28;
  uint32_t d[6];
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base),
                                          dest_texture + kDestFetchOffset + 4 * i),
                4);
    d[i] = __builtin_bswap32(v);
  }
  const TextureFetch fetch = DecodeTextureFetch(d);
  if (!fetch.type_valid || fetch.width == 0 || fetch.height == 0) {
    return;
  }
  // The address the GPU actually writes — and therefore the address a later
  // fetch constant carries — is not the texture base: the guest adds a 4 KiB
  // page depending on bits 20+ before storing it in RB_COPY_DEST_BASE. From
  // sub_82420BA8 verbatim:
  //
  //   v64 = (((v54 >> 20) + 512) & 0x1000) + (v54 & 0x1FFFFFFF)
  //
  // For the shadow atlas that is 0xE6E64000 -> 0x06E64000 + 0x1000 =
  // 0x06E65000, which is exactly the address slot 15 samples in every
  // material shader. Without this the registration misses by one page and
  // every lookup fails.
  const uint32_t raw = fetch.base_address;
  const uint32_t dest = (((raw >> 20) + 512u) & 0x1000u) + (raw & 0x1FFFFFFFu);
  // flags & 7 == 4 selects the depth buffer as the source; 0..3 are colour.
  const bool from_depth = (flags & 7u) == 4u;
  // Every destination, colour included: the address is what marks the data as
  // GPU-produced, and a colour target is invisible to a format-based test.
  NoteFrameCaptureResolve(dest, fetch.width, fetch.height, from_depth);

  // Tie the destination to the target that produced it. The source is
  // whatever the guest was rendering into at this moment, so the render state
  // read here is the same state the draws of that pass used.
  if (!g_draw_ready) {
    return;
  }
  const GuestRenderState rs = ReadRenderState(base, dev);
  const HostViewport hv = ComputeHostViewport(rs);
  RenderTargetKey key;
  key.rt_format = ColorRenderTargetFormatToDxgi(rs.color_format);
  key.ds_format = DepthRenderTargetFormatToDxgi(rs.depth_format);
  key.sample_count = 1;  // pooled targets are single-sampled; see PooledKey
  key.width = uint32_t(hv.top_left_x + hv.width + 0.5f);
  key.height = uint32_t(hv.top_left_y + hv.height + 0.5f);
  if (key.rt_format == 0 || key.width == 0 || key.height == 0) {
    return;
  }
  RenderTarget* source = g_render_targets.Find(key);
  if (!source) {
    // A pass we never rendered; nothing to hand to the destination.
    NoteFrameCaptureResolveMiss(dest, fetch.width, fetch.height, key.width, key.height,
                                key.rt_format, key.ds_format);
    return;
  }
  // The destination is not simply "this target": the shadow map resolves each
  // 640x640 cascade into a quadrant of a 1280x1280 atlas, so the source is
  // SMALLER than the destination and registering the source could never
  // satisfy a fetch for the whole atlas. sub_82420BA8 computes the offset from
  // pSourceRect (a3: x0,y0,x1,y1) and pDestPoint (a5: x,y), so both are needed
  // to place the copy.
  const auto read_be32 = [&](uint32_t ea) -> int32_t {
    if (ea < 0x1000u) {
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
    return int32_t(__builtin_bswap32(v));
  };
  ResolveRegion region;
  if (source_rect) {
    region.src_x = read_be32(source_rect);
    region.src_y = read_be32(source_rect + 4);
    region.width = uint32_t(read_be32(source_rect + 8) - region.src_x);
    region.height = uint32_t(read_be32(source_rect + 12) - region.src_y);
  } else {
    region.width = source->key.width;
    region.height = source->key.height;
  }
  if (dest_point) {
    region.dst_x = read_be32(dest_point);
    region.dst_y = read_be32(dest_point + 4);
  }
  g_render_targets.NoteResolve(*source, from_depth, dest, fetch.width, fetch.height,
                               region);
  // Marked GPU-produced only now, once the copy is actually on its way. Marking
  // it up front — before the `g_draw_ready` guard above — condemned every
  // address we cannot bridge to the neutral fallback forever: resolves that
  // happen once outside a capture window (g_draw_ready still false) registered
  // the address and never produced a resource, so the object sampling it lost
  // its texture entirely instead of decoding guest memory. Bridge when we can,
  // decode when we cannot, and never claim ownership of an image we do not have.
  g_render_targets.NoteDestination(dest, fetch.width, fetch.height);
}

void NoteGuestDraw(int kind) { NoteFrameCaptureGuestDraw(kind); }

namespace {
// Begin/EndVertices are strictly paired on the guest draw thread, so one
// pending slot is enough. Kept out of the device shadow deliberately: the
// guest writes the fetch constant for this buffer straight into the command
// stream, not into the shadow BuildGeometrySnapshot reads.
struct PendingInlineDraw {
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t buffer = 0;  // guest address returned by BeginVertices
  bool valid = false;
};
PendingInlineDraw g_pending_inline;
}  // namespace

void NoteBeginVertices(uint32_t dev, uint32_t primitive_type, uint32_t vertex_count,
                       uint32_t stride, uint32_t buffer) {
  (void)dev;
  g_pending_inline = PendingInlineDraw{primitive_type, vertex_count, stride, buffer,
                                       buffer != 0 && vertex_count != 0 && stride != 0};
}

void NoteEndVertices(const uint8_t* base, uint32_t dev) {
  if (!g_pending_inline.valid) {
    return;
  }
  const PendingInlineDraw d = g_pending_inline;
  g_pending_inline.valid = false;
  // Inline geometry (BeginVertices/EndVertices) is how the guest issues the
  // fullscreen composite/tonemap pass AND the 2D/HUD/UI. It must be recorded in
  // CONTINUOUS mode too, not only the one-shot capture: without it the composite
  // target got only its ~2 bound-stream draws and came out flat (no tonemap, no
  // UI). Mirror TryFirstDraw's mode handling.
  const bool want_continuous = REXCVAR_GET(mcla_native_gfx_continuous);
  const uint32_t capture_limit =
      want_continuous ? 1000000u : uint32_t(REXCVAR_GET(mcla_native_gfx_capture));
  if (capture_limit == 0 || !g_draw_ready || g_draw_init_failed ||
      (!want_continuous && FrameCaptureDone())) {
    return;
  }
  const uint32_t aux_override = uint32_t(REXCVAR_GET(mcla_native_gfx_auxstage));
  const uint32_t aux_stage = want_continuous ? (aux_override ? aux_override : 3u) : aux_override;
  // The RAW return value, not the page-fixed one. sub_8241CD88 applies
  //   (((v >> 20) + 512) & 0x1000) + (v & 0x1FFFFFFF)
  // only to the copy it writes into the FETCH CONSTANT, i.e. the address the
  // GPU reads through. What it returns is the pointer the guest itself writes
  // the vertices through — sub_8217A140 does `*(float *)result = x` on it — so
  // that is the address to read the data back from. Using the fixed one here
  // read a page past the data and crashed with an access violation.
  CaptureInlineDraw(base, dev, d.primitive_type, d.vertex_count, d.stride, d.buffer, capture_limit,
                    g_draw_context, g_draw_shaders, g_buffers, g_textures, g_binder, g_pipelines,
                    g_render_targets, aux_stage);
}

bool ShouldDumpRenderTargets() { return REXCVAR_GET(mcla_native_gfx_dumprt); }

void NotifyFrameBoundary() {
  // TEMP DIAG (remove after): is the frame-boundary hook firing, and do the
  // continuous gates pass?
  {
    static unsigned fb = 0;
    ++fb;
    if (fb <= 10u || (fb % 60u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "frameboundary#%u continuous=%d present_ready=%d draw_ready=%d\n", fb,
                     ContinuousMode() ? 1 : 0, g_present_ready ? 1 : 0, g_draw_ready ? 1 : 0);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // Every retired resource is parked UNTAGGED (sentinel fence ~0) until
  // EndFrameReleases() stamps it, and ReleaseCompleted only frees entries whose
  // fence value the GPU has passed. So a boundary that does not reach a stamp
  // leaks EVERYTHING retired since the last one -- retired buffer regions,
  // evicted textures, orphaned resolve destinations -- for the rest of the
  // session. The call used to sit inside the continuous branch under
  // `g_present_ready && g_draw_ready`, so the one-shot capture mode never
  // stamped at all, and continuous mode did not stamp until the presenter came
  // up. Every path below now ends at a stamp, and it is taken AFTER that path's
  // last submission so the fence value covers the whole frame.
  const auto stamp_releases = [] {
    if (g_draw_ready) {
      g_draw_context.EndFrameReleases();
      MemoryCensusTick(g_draw_context, g_buffers, g_textures, g_pipelines, g_render_targets);
      DumpResolveMisses();
    }
  };
  // Continuous mode, guest thread: submit the frame and publish its display
  // target for the command processor's swap callback to blit. Then reset the
  // per-frame state so the next frame re-accumulates. The actual present
  // happens in the CP's swap (NativeGuestOutputCallback), not here.
  if (ContinuousMode()) {
    if (g_present_ready && g_draw_ready) {
      PrepareContinuousDisplay(g_draw_context, g_render_targets);
      // Programmatic RenderDoc capture of one native frame. The native present
      // bypasses the swapchain, so RenderDoc's Present-driven frame delimiter
      // (and its overlay) stop working once native_gfx takes over -- but the
      // in-application StartFrameCapture/EndFrameCapture bracket submissions on
      // the device directly, no swapchain needed. A capture spans boundary N
      // (Begin, below) to boundary N+1 (End, here), so it records the full
      // native frame between them. Triggered by dropping a file next to the exe
      // (created on demand once the desired scene is on screen), consumed
      // atomically with std::remove so it fires exactly once.
      // The 180+ stacked Begins that produced one 270 MB unreplayable capture
      // did not come from here -- they came from a second, unpaired
      // RenderDocBeginCapture in frame_capture.cpp that continuous mode re-armed
      // every frame. With that gone and RenderDocBeginCapture refusing to nest,
      // this can re-arm: the latch now trips only when EndFrameCapture actually
      // fails to close, which is the case the latch was protecting against.
      // Drop the trigger file again for another capture.
      static bool rdc_capturing = false;
      static bool rdc_done = false;
      if (rdc_capturing) {
        rdc_capturing = false;
        if (!RenderDocEndCapture(g_draw_context.device())) {
          rdc_done = true;
        }
      }
      // After PrepareContinuousDisplay: that call makes the frame's LAST
      // submission, so the fence value stamped here covers every batch that
      // could still reference a resource retired during this frame.
      stamp_releases();
      // Flip the descriptor heaps to the other half and drop the per-frame SRV/
      // sampler caches so next frame allocates fresh descriptors (a freed+recycled
      // resource can no longer alias a stale pointer-keyed SRV) without clobbering
      // this frame's still-in-flight half.
      g_binder.BeginFrame();
      ResetContinuousFrame(g_render_targets);
      // Arm the next frame's capture if the trigger file is present. std::remove
      // returns 0 only when it existed and was deleted, consuming it atomically.
      // Two producers, one consumer: the keybind (any thread, atomic flag) and
      // the trigger file kept for scripts. exchange/remove both consume once.
      const bool key_request = g_rdc_capture_request.exchange(false, std::memory_order_acq_rel);
      const bool file_request = std::remove("native_gfx_rdc_trigger") == 0;
      if (!rdc_capturing && !rdc_done && (key_request || file_request)) {
        if (RenderDocBeginCapture(g_draw_context.device())) {
          rdc_capturing = true;
          REXLOG_INFO("[native_gfx] RenderDoc capture armed for the next guest frame");
        } else {
          REXLOG_ERROR(
              "[native_gfx] RenderDoc capture could not be started (not running under RenderDoc, "
              "or a capture is already open)");
        }
      }
    } else {
      // Continuous mode with no frame submitted this boundary (the presenter is
      // not up yet). Nothing new references what was retired, so the last
      // submitted fence value is already a safe stamp.
      stamp_releases();
    }
    return;
  }
  stamp_releases();
  if (REXCVAR_GET(mcla_native_gfx_capture) > 0) {
    // Skip the first mcla_native_gfx_capture_delay frame boundaries before
    // arming, so the capture lands on a frame where the LZX texture streaming
    // has caught up and the scene is fully detailed (early frames show
    // low-detail placeholders and unconverged exposure -> a dark, muddy image).
    static uint32_t skipped = 0;
    const uint32_t delay = uint32_t(REXCVAR_GET(mcla_native_gfx_capture_delay));
    if (skipped < delay) {
      ++skipped;
      return;
    }
    ArmFrameCapture();
    SetFrameCaptureDumpTargets(REXCVAR_GET(mcla_native_gfx_dumprt));
  }
}

void RequestRenderDocCapture() {
  if (!RenderDocAvailable()) {
    REXLOG_ERROR("[native_gfx] RenderDoc is not attached to this process; cannot capture");
    return;
  }
  g_rdc_capture_request.store(true, std::memory_order_release);
  REXLOG_INFO("[native_gfx] RenderDoc capture requested");
}

// Legacy no-op: continuous present moved into the command processor's native
// guest-output callback. Kept so the swap hook still links.
void PresentContinuousAtSwap() {}

bool PresentTakeover() { return Active() && REXCVAR_GET(mcla_native_gfx_present); }

bool PresentFrame() {
  if (!PresentTakeover()) {
    return false;
  }
  return g_triangle.Present(g_presenter);
}

}  // namespace mcla::native_gfx

// NOTE: the D3DDevice_Swap hook (rex_sub_82419E98) lives in hooks.cpp, the
// single owner of the guest graphics hooks, together with the draw, tiling
// and frame-boundary hooks shared with the passive probe.

#endif // REXGLUE_HAS_XEO3_TARGET
