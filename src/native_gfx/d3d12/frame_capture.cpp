#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — multi-draw frame capture.
// See frame_capture.h for why draws are grouped by render target config.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "frame_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/flags.h>

#include "../draw_slicing.h"
#include "../geometry.h"
#include "../guest/guest_constants.h"
#include "../guest/guest_resources.h"
#include "../guest/render_state.h"
#include "../native_gfx.h"
#include "../shader_identity.h"
#include "constant_upload.h"
#include "context.h"
#include "image_dump.h"
#include "pipeline_cache.h"
#include "render_target_pool.h"
#include "renderdoc_hook.h"
#include "resource_cache.h"
#include "shader_db.h"
#include "texture_binding.h"
#include "texture_cache.h"
#include <rex/runtime.h>

#include "fxaa_pass.h"
#include "gamma_pass.h"
#include "tonemap_pass.h"
#include "topology_expand.h"
#include "blit_pass.h"
#include "presenter_output.h"

#include <rex/ui/d3d12/d3d12_presenter.h>

REXCVAR_DEFINE_DOUBLE(mcla_native_gfx_exposure, 8.0, "MCLA/NativeGfx",
                      "Exposure multiplier applied by the present-time tonemap (HDR anchor -> "
                      "LDR). The raw scene target is very dark (linear, unconverged exposure); "
                      "this lifts it before the ACES curve and gamma. Only used when "
                      "mcla_native_gfx_present_anchor is on.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_fxaa, false, "MCLA/NativeGfx",
                    "Run NVIDIA FXAA over the composite at present. The console had no "
                    "anti-aliasing to inherit here -- the game renders unresolved -- so this "
                    "is an addition, not a fidelity fix, and it is a whole-frame filter that "
                    "deserves its own A/B. It goes before the display gamma ramp, which is "
                    "where the console's own ramp sits (the DC_LUT is scanout, after "
                    "everything the GPU drew). Independent of mcla_native_gfx_msaa: MSAA "
                    "resolves geometry edges only, FXAA also catches shader and alpha-test "
                    "edges, and the two compose. Ignored on the present_anchor path, whose "
                    "source is the raw HDR scene target.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// TEMP DIAG helper for the rim probe: IEEE half -> float. first_draw.cpp has one
// but it is not exported, and this is scaffolding that leaves with the probe.
static float RimHalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t man = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {
      int e = -1;
      uint32_t m = man;
      do {
        ++e;
        m <<= 1;
      } while ((m & 0x400u) == 0);
      bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | (exp + 127 - 15) << 23 | (man << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

REXCVAR_DEFINE_UINT32(mcla_native_gfx_shadow_quads, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: check this many times whether the four 640x640 quadrants of "
                      "the resolved shadow atlas (0x06E65000) are distinct, one check every 180 "
                      "published frames. Two identical quadrants mean a cascade inherited the "
                      "previous cascade depth and rejected every fragment -- the 'sun only "
                      "inside a box' failure, which on the GPS map (the 3D city at low LOD) "
                      "reads as a whole map with no sun. Reads the RESOLVED copy: a pooled "
                      "1280x1280 hands back its clear colour through recycling and would read "
                      "as a false negative.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_exposure_probe, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: read back the auto-exposure targets (1x1 at 0x02D6C000 and "
                      "4x4 at 0x02D6D000) this many times, one every 200 published frames, as "
                      "raw floats. The native path renders cam 53 with its midtones +84 and "
                      "11.6% of the frame clipped white against the emulated path's 2.6%, with "
                      "matching shadows -- the shape of a gain before the tonemap.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_lowres_addr, 0x02DE6000, "MCLA/NativeGfx",
                      "TEMP DIAG: guest address mcla_native_gfx_lowres_dump reads.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(mcla_native_gfx_lowres_w, 320, "MCLA/NativeGfx",
                      "TEMP DIAG: width for mcla_native_gfx_lowres_dump.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(mcla_native_gfx_lowres_h, 180, "MCLA/NativeGfx",
                      "TEMP DIAG: height for mcla_native_gfx_lowres_dump.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_lowres_dump, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: dump this many raw copies of the guest memory behind the "
                      "320x180 post-process buffer at 0x02DE6000, one every 300 published "
                      "frames. That buffer's resolve misses every frame (the key asks for "
                      "R8G8B8A8, the only 320x180 pass in the pool is the HDR one), so a fetch "
                      "of it reads whatever the title left in memory.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_rim_probe, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: report this many xRimMain draws -- the vertex declaration, "
                      "TEXCOORD0 of the first vertices, and VS constants 206..215 (tintColors). "
                      "The wheel picks its paint slot with a0 = trunc(TEXCOORD0.z), so a wrong "
                      "third component reads a neighbouring part's colour.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_aniso_census, false, "MCLA/NativeGfx",
                    "TEMP DIAG: count texture binds by whether the sampler is eligible for "
                    "anisotropy and whether the texture actually has a mip chain to filter "
                    "down. Anisotropy picks a finer level along the major axis, so a "
                    "single-level texture makes a correct 16x sampler change nothing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_ps_census, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: every distinct pixel shader the scene runs -- draw count, the "
                      "vertex declaration's COLOR0 format and offset, and the first bound "
                      "textures -- dumped every N draws. The runtime's shader identity is NOT "
                      "the identity column of shader_report.tsv; translate these with "
                      "build_shader_pack.collect_shaders(). This is what NAMES an unknown "
                      "material: run the scene that shows the bug and read the list.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_eye_probe, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: report this many draws of the character eye/teeth material "
                      "(xCharacter_teeth_normalmap) with its tintColor constant and its bound "
                      "textures. Chasing eyes that render black in the native path and normal "
                      "in the emulated one.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_native_gfx_fxaa_dump, 0, "MCLA/NativeGfx",
                      "TEMP DIAG: write this many pre/post TGA pairs of the FXAA pass "
                      "(native_gfx_fxaa_pre_N.tga / _post_N.tga). Both come from the same "
                      "frame and the same submission, so the difference between them is the "
                      "filter and nothing else.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(mcla_native_gfx_fxaa_threshold, 0.125, "MCLA/NativeGfx",
                      "FXAA edge threshold: the minimum local luma contrast, as a fraction of "
                      "the brighter luma, before a pixel is filtered at all. Lower catches "
                      "more edges and softens more of the frame; 0.333 is NVIDIA's fastest "
                      "preset, 0.125 the default quality one, 0.063 the most aggressive.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(mcla_native_gfx_fxaa_subpixel, 0.75, "MCLA/NativeGfx",
                      "How much of FXAA's sub-pixel term is allowed through. That term is what "
                      "handles thin features and lone pixels the edge walk cannot resolve -- "
                      "wires, railings, distant lamp posts -- and it is also the part that "
                      "blurs. 0 turns it off and keeps the image sharpest, 1 is the softest.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_present_anchor, false, "MCLA/NativeGfx",
                    "Continuous mode: present the anchor (raw scene target) instead of the "
                    "readback (composite) pass. The continuous readback comes out flat/black "
                    "while the anchor holds the real rendered scene, so this makes the native "
                    "world visible while the composite-selection bug is still open.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_drawcap, 0, "MCLA/NativeGfx",
                      "Continuous mode: cap the number of native ANCHOR (main scene) draws "
                      "recorded per frame. 0 = uncapped. A low value (e.g. 300) tests whether "
                      "the DEVICE_HUNG TDR is aggregate GPU load rather than one bad draw: if "
                      "capping stops the crash, it is load. Also a direct performance lever.");

REXCVAR_DEFINE_UINT32(mcla_native_gfx_auxcap, 8192, "MCLA/NativeGfx",
                      "Per-frame cap on AUXILIARY draws (shadow, light, reflection passes) the "
                      "native runtime records. 0 = uncapped. Was a hardcoded 2600, which "
                      "measurement showed saturating in dense scenes and silently dropping the "
                      "very lighting passes it bounded. A cap still exists because the documented "
                      "failure is aggregate GPU load exceeding the TDR window on weak hardware, "
                      "not one resource running out -- lower it if frame time regresses. The "
                      "shipped default should come from the measured peak (peak_aux in the prepare "
                      "log), not a guess; 8192 is a deliberately loose value for that measurement.");

REXCVAR_DEFINE_BOOL(mcla_native_gfx_hangfind, false, "MCLA/NativeGfx",
                    "Diagnostic: in continuous mode, submit each recorded draw on its own and "
                    "wait for it with a timeout. The draw whose shader spins forever (the "
                    "DEVICE_HUNG TDR) is the one that times out; its vs/ps identity is written "
                    "to native_gfx_hang.txt. Extremely slow — a one-shot to name the culprit.");

REXCVAR_DECLARE(bool, mcla_native_gfx_alpha_ref);
REXCVAR_DECLARE(bool, mcla_native_gfx_guest_clear);
REXCVAR_DECLARE(bool, mcla_native_gfx_reclear);
REXCVAR_DECLARE(bool, mcla_native_gfx_skip_punch);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_skip_water);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_msaa);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_mrt);
REXCVAR_DECLARE(bool, mcla_native_gfx_surface_key);
REXCVAR_DECLARE(bool, mcla_native_gfx_gamma_ramp);
REXCVAR_DECLARE(bool, mcla_native_gfx_unsupplied_drop);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_skip_draw_first);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_skip_draw_last);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_dump_draw_first);
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_dump_draw_last);
REXCVAR_DECLARE(bool, mcla_native_gfx_half_pixel);
REXCVAR_DECLARE(bool, mcla_native_gfx_swapped_texcoords);

namespace mcla::native_gfx {

namespace {

constexpr float kClearColor[4] = {0.02f, 0.02f, 0.04f, 0.0f};
// The guest viewport decides the target size; this only bounds a nonsensical
// register read so a bad value cannot ask for a gigabyte of render target.
constexpr uint32_t kMaxTargetDimension = 4096;

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

// Everything that has to match for two draws to belong to the same pass.
struct TargetConfig {
  uint32_t rt_format = 0;
  // What the guest asked for, as opposed to what the pool allocates. See
  // RenderTargetKey.
  uint32_t guest_msaa = 0;
  uint32_t surface_pitch = 0;
  // Second colour target's DXGI format, 0 when the pass writes only oC0. The
  // impostor bake is the one pass in MCLA that sets it; see kDevRegColorInfo1.
  uint32_t rt1_format = 0;
  uint32_t ds_format = 0;
  uint32_t sample_count = 1;
  uint32_t width = 0;
  uint32_t height = 0;
  bool operator==(const TargetConfig& o) const {
    return rt_format == o.rt_format && rt1_format == o.rt1_format &&
           guest_msaa == o.guest_msaa && surface_pitch == o.surface_pitch &&
           ds_format == o.ds_format && sample_count == o.sample_count && width == o.width &&
           height == o.height;
  }
};

struct Capture {
  bool armed = false;    // a frame boundary was crossed; record from here
  // A capture ends at the NEXT frame boundary, not at a draw count. Ending on
  // the draw limit stopped mid-scene, before the guest ever reached the
  // composite pass, so the only full-res image available to read back was the
  // raw HDR scene target. The limit stays as a safety bound on time and memory.
  bool end_requested = false;
  bool started = false;
  bool finished = false;
  bool continuous = false;  // present every frame instead of a one-shot capture
  FILE* log = nullptr;
  std::filesystem::path dir;
  TargetConfig config;

  // The pass whose image is read back. Other passes still render — they
  // produce the shadow atlas and the auxiliary buffers this one samples.
  RenderTargetKey anchor_key;
  bool has_anchor = false;

  // The image read back is NOT the anchor. The anchor is the HDR scene target
  // (1280x720 rt_format=10, R16G16B16A16_FLOAT) — the frame before tonemapping,
  // before the composite, before every lighting resolve is applied. The guest
  // then runs a full-res LDR pass (rt_format=28) that samples it and produces
  // the image actually displayed. Reading the anchor back showed the raw scene
  // buffer, which is why enabling the auxiliary passes changed the shadow atlas
  // and the collector without changing a single lit pixel in the dump.
  //
  // Kept separate from the anchor rather than moving the anchor, because the
  // anchor also decides which draws count against the scene's draw budget: the
  // composite is ~86 draws and the scene is thousands, so making the composite
  // the anchor would push the whole scene into the auxiliary cap.
  RenderTargetKey readback_key;
  TargetConfig readback_config;
  bool has_readback = false;

  // Draws are recorded into ONE command list and submitted in batches.
  // Submitting per draw costs a fence wait each time on the queue shared with
  // the Xenia command processor, and with every pass enabled that exceeded the
  // TDR window: measured DXGI_ERROR_DEVICE_HUNG 0x887A0001, with the first
  // creation failure returning DEVICE_REMOVED rather than an OOM.
  bool renderdoc_capturing = false;
  bool frame_open = false;
  uint32_t draws_in_batch = 0;

  uint32_t limit = 0;
  uint32_t offered = 0;
  // Draws the bisection filter removed this frame. Reset per frame like
  // `offered` and `accepted`, unlike the rej_* counters below, which are
  // cumulative for the session.
  uint32_t skipped_by_range = 0;
  // Quad-expanded draws whose vertex buffer was rebuilt one-source-vertex-per-
  // quad-corner, and the ones where that could not be done (guest range not
  // committed, or the upload ring full). A failure here is the black wedge
  // coming back for that draw, so the two are counted apart.
  uint32_t quad_replicated = 0;
  uint32_t quad_replicate_failed = 0;
  // Expanded draws whose guest start vertex is not zero -- the offset the
  // expanded path currently drops. Diagnostic only.
  uint32_t expanded_nonzero_start = 0;
  // Why a fold rebuild gave up: source range past the fetch size, guest range
  // unreadable, upload ring refused, no foldable stream at all.
  uint32_t quad_fail_size = 0;
  uint32_t quad_fail_read = 0;
  uint32_t quad_fail_alloc = 0;
  uint32_t quad_fail_nostream = 0;
  uint32_t accepted = 0;          // anchor pass; drives the limit
  uint32_t accepted_aux = 0;      // shadow/effect auxiliary passes, shared budget
  uint32_t accepted_composite = 0;  // display-shaped LDR composite/UI pass, own budget
  uint32_t aux_logged = 0;
  bool dump_render_targets = false;
  // Rejection tallies. Without these a low accepted count is indistinguishable
  // from a pipeline that silently drops most of the frame.
  uint32_t rej_not_indexed = 0;
  uint32_t rej_topology = 0;
  uint32_t rej_no_shader = 0;
  uint32_t rej_geometry = 0;
  uint32_t rej_unsupplied = 0;
  // Draws thrown away between `offered` and any rejection reason. Measured:
  // offered=3723 accepted=3337 with every rej_* counter at zero, so 386 draws
  // per report were vanishing with nothing to say where. That gap is the only
  // place a missing-geometry artefact can hide from the log.
  uint32_t rej_not_armed = 0;
  // Expanded draws whose indices would read past the vertex fetch. See the
  // rejection site for why drawing them is worse than not.
  uint32_t rej_outruns_fetch = 0;
  uint32_t rej_config = 0;
  // rej_config lumps three unrelated causes together, which hid that the aux
  // budget alone accounts for most of it. Split so each can be judged.
  uint32_t rej_cfg_target = 0;   // unusable TargetConfig (format/dimensions)
  uint32_t rej_cfg_budget = 0;   // aux/composite per-frame draw cap reached
  uint32_t rej_cfg_patho = 0;    // pathological element count / index range
  uint32_t rej_unsup_display = 0;  // unsupplied attribute on a DISPLAY-shaped draw
  uint32_t rej_shader_missing = 0;
  uint32_t fail_bind = 0;
  uint32_t fail_pso = 0;
  uint32_t fail_constants = 0;
  // Batches submitted early because the per-slot upload ring ran out. Non-zero
  // means the ring is the binding constraint, not the draw cap.
  uint32_t ring_flushes = 0;
  // Highest accepted_aux seen this session. accepted_aux is reset every frame,
  // so without this the true peak is unobservable -- and the peak is what the
  // aux cap default has to be chosen from.
  uint32_t peak_aux = 0;
  uint32_t peak_composite = 0;
};

// Snapshot of the binder's counters, refreshed as draws are recorded.
// PrepareContinuousDisplay writes the prepare line but has no TextureBinder in
// scope, and descriptor exhaustion is exactly the failure that must not stay
// invisible while the draw cap is being raised.
TextureBinder::Stats g_binder_stats_for_report;
RenderTargetKey g_minimap_key;
bool g_minimap_seen = false;
// TEMP INSTRUMENTATION: the per-frame CPU line names WHERE the time goes
// (geom/bind) but not WHY. These carry the two caches' counters to the report
// site, which sees neither object, so a frame can say whether geom is paying
// for real first-use uploads, for invalidation-driven re-uploads, or for cache
// hits that are simply slow.
BufferCache::Stats g_buffer_stats_for_report;
TextureCache::Stats g_texture_stats_for_report;

Capture g_cap;

// Defined here rather than further down: every reporting helper below needs it.
#define CLOGF(...)                          \
  do {                                      \
    if (g_cap.log) {                        \
      std::fprintf(g_cap.log, __VA_ARGS__); \
      std::fflush(g_cap.log);               \
    }                                       \
  } while (0)

// Per-frame CPU breakdown of the draw path. `wall` and `gpu_wait` in the
// prepare line answer "CPU or GPU?"; these answer "which part of the CPU?".
// Six steady_clock reads per draw is well under a microsecond against the tens
// this path costs, so it stays always-on rather than behind a switch that would
// need a rebuild the moment a frame time regresses.
struct DrawProfile {
  double state_us = 0;   // render state + reject-time geometry snapshot
  double shader_us = 0;  // microcode identity + pack lookup
  double geom_us = 0;    // binding geometry snapshot (vertex/index upload)
  double bind_us = 0;    // texture/sampler binding
  double const_us = 0;   // constant banks: read, compare, upload
  double pso_us = 0;     // PSO key + cache
};
DrawProfile g_profile;

using ProfileClock = std::chrono::steady_clock;
inline void ProfileAdd(double& slot, ProfileClock::time_point begin) {
  slot += std::chrono::duration<double, std::micro>(ProfileClock::now() - begin).count();
}

// Histogram of the render target configurations that were offered but not
// captured. Without it, a large "other_target" count says nothing about what
// the rest of the frame actually is.
struct RejectedConfig {
  TargetConfig cfg;
  uint32_t count = 0;
};
constexpr uint32_t kMaxRejectedConfigs = 12;
RejectedConfig g_rejected[kMaxRejectedConfigs];
uint32_t g_rejected_count = 0;

// Per-format tally of the textures the draws asked for, split by whether the
// cache could serve them. A big surface rendering flat could mean the texture
// was never resolved, or that it resolved to the wrong image; only the split
// tells the two apart.
struct FormatTally {
  uint32_t format = 0xFFFFFFFFu;
  uint32_t resolved = 0;
  uint32_t unresolved = 0;
};

// The individual fetches that could not be resolved, keyed by address, so the
// missing resource can be identified rather than merely counted.
struct UnresolvedFetch {
  uint32_t address = 0;
  uint32_t width = 0, height = 0, format = 0, slot = 0;
  bool tiled = false;
  uint32_t count = 0;
};
constexpr uint32_t kMaxUnresolved = 32;
UnresolvedFetch g_unresolved[kMaxUnresolved];
uint32_t g_unresolved_count = 0;

void NoteUnresolved(const BoundTexture& b) {
  for (uint32_t i = 0; i < g_unresolved_count; ++i) {
    if (g_unresolved[i].address == b.fetch.base_address) {
      ++g_unresolved[i].count;
      return;
    }
  }
  if (g_unresolved_count < kMaxUnresolved) {
    UnresolvedFetch& u = g_unresolved[g_unresolved_count++];
    u.address = b.fetch.base_address;
    u.width = b.fetch.width;
    u.height = b.fetch.height;
    u.format = b.fetch.format;
    u.slot = b.fetch_slot;
    u.tiled = b.fetch.tiled;
    u.count = 1;
  }
}
constexpr uint32_t kMaxFormats = 24;
FormatTally g_formats[kMaxFormats];
uint32_t g_format_count = 0;

void NoteTexture(uint32_t format, bool resolved) {
  for (uint32_t i = 0; i < g_format_count; ++i) {
    if (g_formats[i].format == format) {
      resolved ? ++g_formats[i].resolved : ++g_formats[i].unresolved;
      return;
    }
  }
  if (g_format_count < kMaxFormats) {
    g_formats[g_format_count].format = format;
    g_formats[g_format_count].resolved = resolved ? 1u : 0u;
    g_formats[g_format_count].unresolved = resolved ? 0u : 1u;
    ++g_format_count;
  }
}

// Resolves the guest performed while the capture was running. The address a
// resolve writes to is exactly the address a later texture fetch reads, so
// this table is what ties a render target to the texture that samples it.
struct ResolveRecord {
  uint32_t address = 0;
  uint32_t width = 0, height = 0;
  bool from_depth = false;
  uint32_t count = 0;
};
struct ResolveMissRecord {
  uint32_t address, dest_width, dest_height, src_width, src_height;
  uint32_t rt_format, ds_format, count;
};
constexpr uint32_t kMaxResolveMisses = 32;
ResolveMissRecord g_resolve_misses[kMaxResolveMisses];
uint32_t g_resolve_miss_count = 0;

struct PassTally {
  uint32_t sig = 0, width = 0, height = 0, rt_format = 0;
  uint32_t draws = 0, draws_writing = 0, mask_or = 0;
};
PassTally g_pass_tallies[8];
uint32_t g_pass_tally_count = 0;

void NotePassDraw(uint32_t width, uint32_t height, uint32_t rt_format, uint32_t color_mask) {
  const uint32_t sig = width * 31u + height * 7u + rt_format;
  PassTally* t = nullptr;
  for (uint32_t k = 0; k < g_pass_tally_count; ++k) {
    if (g_pass_tallies[k].sig == sig) {
      t = &g_pass_tallies[k];
      break;
    }
  }
  if (!t) {
    if (g_pass_tally_count >= 8) {
      return;
    }
    t = &g_pass_tallies[g_pass_tally_count++];
    *t = PassTally{sig, width, height, rt_format, 0, 0, 0};
  }
  ++t->draws;
  t->mask_or |= color_mask;
  if (color_mask & 0xFu) {
    ++t->draws_writing;
  }
}

// TEMP INSTRUMENTATION: guest draw entry points, indexed / non-indexed / UP.
uint32_t g_guest_draws[3];

// Inline draws, tallied by primitive type so the RECTLIST share is visible.
uint32_t g_inline_draws = 0;
uint32_t g_inline_verts = 0;
uint32_t g_inline_by_prim[16];
// Which primitive types PrimitiveTypeToTopology has no mapping for. Without
// the breakdown a topology rejection count says nothing about what to add.
uint32_t g_rejected_topology_by_prim[16];

// TEMP INSTRUMENTATION: full per-draw state for the shadow pass, so a draw
// that reaches the atlas can be compared field by field against one that does
// not. Recorded for every shadow draw and reported for the extremes by element
// count, because the atlas shows thin geometry (poles, wires) and nothing
// large.
struct ShadowDrawRecord {
  uint32_t index = 0;
  uint32_t element_count = 0, start_element = 0;
  int32_t base_vertex = 0;
  uint32_t primitive_type = 0;
  bool indexed = false;
  uint64_t vs_id = 0, ps_id = 0;
  // Viewport / scissor.
  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0, vp_min_z = 0, vp_max_z = 0;
  bool y_flipped = false;
  uint32_t scissor_w = 0, scissor_h = 0;
  // Raster / depth / blend, raw registers plus the decoded fields the PSO uses.
  uint32_t pa_su_sc_mode_cntl = 0, depth_control = 0, color_mask = 0;
  uint32_t blend_control0 = 0, color_control = 0, mode_control = 0;
  bool cull_front = false, cull_back = false, front_face_is_cw = false;
  bool depth_enable = false, depth_write = false, stencil_enable = false;
  uint32_t depth_func = 0;
  // Target.
  uint32_t rt_format = 0, ds_format = 0, sample_count = 0;
  float clear_depth = 0;
  // Geometry.
  uint32_t stream_count = 0, layout_elements = 0;
  uint32_t stride0 = 0, base0 = 0, size0 = 0, endian0 = 0;
  bool index_32bit = false;
  uint32_t index_bytes = 0;
  uint64_t pso_key_hash = 0;
  // The first eight VS ALU constants: c0..c3 is the world-view-projection in
  // RAGE's shaders, c4..c7 whatever follows it.
  float vs_c[32] = {};
};
constexpr uint32_t kMaxShadowRecords = 2048;
ShadowDrawRecord g_shadow_records[kMaxShadowRecords];
uint32_t g_shadow_record_count = 0;

const char* CompareFuncName(uint32_t f) {
  static const char* kNames[8] = {"NEVER",     "LESS",     "EQUAL",  "LESS_EQUAL",
                                  "GREATER",   "NOT_EQUAL","GREATER_EQUAL", "ALWAYS"};
  return kNames[f & 7u];
}

void PrintShadowDraw(const ShadowDrawRecord& r, const char* tag) {
  CLOGF("\n  --- %s: shadow draw #%u, %u elements ---\n", tag, r.index, r.element_count);
  CLOGF("    shaders        vs=%016llX ps=%016llX%s\n", (unsigned long long)r.vs_id,
        (unsigned long long)r.ps_id, r.ps_id == 0 ? "  (depth-only)" : "");
  CLOGF("    draw           prim=%u indexed=%d start=%u base_vertex=%d index32=%d index_bytes=%u\n",
        r.primitive_type, r.indexed ? 1 : 0, r.start_element, r.base_vertex, r.index_32bit ? 1 : 0,
        r.index_bytes);
  CLOGF("    geometry       streams=%u layout_elements=%u stride0=%u base0=0x%08X size0=%u "
        "endian0=%u\n",
        r.stream_count, r.layout_elements, r.stride0, r.base0, r.size0, r.endian0);
  CLOGF("    viewport       x=%.1f y=%.1f w=%.1f h=%.1f depth[%.4f..%.4f] y_flipped=%d\n", r.vp_x,
        r.vp_y, r.vp_w, r.vp_h, r.vp_min_z, r.vp_max_z, r.y_flipped ? 1 : 0);
  CLOGF("    scissor        %ux%u   target rt=%u ds=%u samples=%u clear_depth=%.2f\n", r.scissor_w,
        r.scissor_h, r.rt_format, r.ds_format, r.sample_count, r.clear_depth);
  CLOGF("    rasterizer     PA_SU_SC_MODE_CNTL=0x%08X cull_front=%d cull_back=%d front_cw=%d\n",
        r.pa_su_sc_mode_cntl, r.cull_front ? 1 : 0, r.cull_back ? 1 : 0,
        r.front_face_is_cw ? 1 : 0);
  CLOGF("                   depth_bias=NOT READ  slope_bias=NOT READ  depth_clip=NOT READ\n");
  CLOGF("    depth          RB_DEPTHCONTROL=0x%08X enable=%d write=%d func=%s stencil=%d\n",
        r.depth_control, r.depth_enable ? 1 : 0, r.depth_write ? 1 : 0,
        CompareFuncName(r.depth_func), r.stencil_enable ? 1 : 0);
  CLOGF("    blend          RB_BLENDCONTROL0=0x%08X RB_COLORCONTROL=0x%08X RB_COLOR_MASK=0x%08X\n",
        r.blend_control0, r.color_control, r.color_mask);
  CLOGF("    mode           RB_MODECONTROL=0x%08X\n", r.mode_control);
  CLOGF("    pso key hash   %016llX\n", (unsigned long long)r.pso_key_hash);
  for (uint32_t c = 0; c < 8; ++c) {
    CLOGF("    vs c%-2u        % .6f % .6f % .6f % .6f\n", c, r.vs_c[c * 4 + 0], r.vs_c[c * 4 + 1],
          r.vs_c[c * 4 + 2], r.vs_c[c * 4 + 3]);
  }
}

void ReportShadowDraws() {
  if (g_shadow_record_count == 0) {
    return;
  }
  uint32_t largest = 0, smallest = 0;
  for (uint32_t i = 1; i < g_shadow_record_count; ++i) {
    if (g_shadow_records[i].element_count > g_shadow_records[largest].element_count) {
      largest = i;
    }
    if (g_shadow_records[i].element_count < g_shadow_records[smallest].element_count) {
      smallest = i;
    }
  }
  CLOGF("\n=== SHADOW PASS STATE (%u draws recorded) ===\n", g_shadow_record_count);
  PrintShadowDraw(g_shadow_records[largest], "LARGEST (should cast: terrain/building)");
  PrintShadowDraw(g_shadow_records[smallest], "SMALLEST (thin geometry, visible in atlas)");

  // Every field that differs between the two, so the divergence is stated and
  // not left to be eyeballed across the two blocks above.
  const ShadowDrawRecord& a = g_shadow_records[largest];
  const ShadowDrawRecord& b = g_shadow_records[smallest];
  CLOGF("\n  --- DIFFERENCES (largest vs smallest) ---\n");
  uint32_t diffs = 0;
#define DIFF_U(field, fmt)                                                     \
  if (a.field != b.field) {                                                    \
    CLOGF("    " #field ": " fmt " vs " fmt "\n", a.field, b.field);           \
    ++diffs;                                                                   \
  }
  DIFF_U(primitive_type, "%u")
  DIFF_U(indexed, "%d")
  DIFF_U(index_32bit, "%d")
  DIFF_U(stream_count, "%u")
  DIFF_U(layout_elements, "%u")
  DIFF_U(stride0, "%u")
  DIFF_U(endian0, "%u")
  DIFF_U(pa_su_sc_mode_cntl, "0x%08X")
  DIFF_U(depth_control, "0x%08X")
  DIFF_U(color_mask, "0x%08X")
  DIFF_U(blend_control0, "0x%08X")
  DIFF_U(color_control, "0x%08X")
  DIFF_U(mode_control, "0x%08X")
  DIFF_U(cull_front, "%d")
  DIFF_U(cull_back, "%d")
  DIFF_U(front_face_is_cw, "%d")
  DIFF_U(depth_enable, "%d")
  DIFF_U(depth_write, "%d")
  DIFF_U(depth_func, "%u")
  DIFF_U(stencil_enable, "%d")
  DIFF_U(rt_format, "%u")
  DIFF_U(ds_format, "%u")
  DIFF_U(sample_count, "%u")
  DIFF_U(scissor_w, "%u")
  DIFF_U(scissor_h, "%u")
  DIFF_U(y_flipped, "%d")
#undef DIFF_U
#define DIFF_F(field)                                                          \
  if (a.field != b.field) {                                                    \
    CLOGF("    " #field ": %.4f vs %.4f\n", a.field, b.field);                 \
    ++diffs;                                                                   \
  }
  DIFF_F(vp_x)
  DIFF_F(vp_y)
  DIFF_F(vp_w)
  DIFF_F(vp_h)
  DIFF_F(vp_min_z)
  DIFF_F(vp_max_z)
  DIFF_F(clear_depth)
#undef DIFF_F
  if (a.vs_id != b.vs_id) {
    CLOGF("    vs_id: %016llX vs %016llX\n", (unsigned long long)a.vs_id,
          (unsigned long long)b.vs_id);
    ++diffs;
  }
  if (a.pso_key_hash != b.pso_key_hash) {
    CLOGF("    pso_key_hash: %016llX vs %016llX\n", (unsigned long long)a.pso_key_hash,
          (unsigned long long)b.pso_key_hash);
    ++diffs;
  }
  if (diffs == 0) {
    CLOGF("    none in recorded pipeline state; the difference is in the "
          "constants or the vertex data\n");
  }

  // Distribution: if only a handful of distinct shaders or PSOs appear across
  // ~1000 draws, the pass is uniform and the divergence is per-draw data.
  CLOGF("\n  --- shadow pass distribution ---\n");
  uint64_t distinct_vs[16] = {};
  uint32_t distinct_vs_count = 0;
  uint32_t elem_min = 0xFFFFFFFFu, elem_max = 0, elem_over_1000 = 0;
  uint64_t elem_total = 0;
  for (uint32_t i = 0; i < g_shadow_record_count; ++i) {
    const ShadowDrawRecord& r = g_shadow_records[i];
    elem_min = r.element_count < elem_min ? r.element_count : elem_min;
    elem_max = r.element_count > elem_max ? r.element_count : elem_max;
    elem_total += r.element_count;
    if (r.element_count >= 1000) {
      ++elem_over_1000;
    }
    bool seen = false;
    for (uint32_t k = 0; k < distinct_vs_count; ++k) {
      if (distinct_vs[k] == r.vs_id) {
        seen = true;
        break;
      }
    }
    if (!seen && distinct_vs_count < 16) {
      distinct_vs[distinct_vs_count++] = r.vs_id;
    }
  }
  CLOGF("    elements: min=%u max=%u mean=%llu   draws with >=1000 elements: %u\n", elem_min,
        elem_max, (unsigned long long)(elem_total / g_shadow_record_count), elem_over_1000);
  CLOGF("    distinct vertex shaders: %u\n", distinct_vs_count);
}

// Index buffers for the primitives D3D12 cannot express. Keyed only by
// (primitive type, vertex count), so it is shared across draws and frames and
// never depends on vertex data.
TopologyExpander g_topology;
uint32_t g_expanded_draws = 0;
uint32_t g_expanded_indices = 0;
// Vertices the host synthesises (rect lists). Reset per capture, not per draw:
// the allocations must stay alive until the batch that references them is
// submitted.
InlineVertexRing g_inline_vertices;
bool g_dumped_collector = false;  // TEMP INSTRUMENTATION: one-shot slot-13 dump
bool g_dumped_composite = false;  // TEMP INSTRUMENTATION: one-shot composite dump
uint32_t g_composite_lines = 0;   // TEMP INSTRUMENTATION: composite-pass draw log
uint32_t g_exposure_lines = 0;    // TEMP INSTRUMENTATION: exposure-chain draw log

// All of the above are per-frame: cleared when a capture starts, so the
// figures describe one frame rather than everything since process start.
void ResetPerFrameTallies() {
  g_inline_draws = 0;
  g_inline_verts = 0;
  std::memset(g_inline_by_prim, 0, sizeof(g_inline_by_prim));
  std::memset(g_rejected_topology_by_prim, 0, sizeof(g_rejected_topology_by_prim));
  std::memset(g_guest_draws, 0, sizeof(g_guest_draws));
  g_pass_tally_count = 0;
  g_expanded_draws = 0;
  g_expanded_indices = 0;
  // Safe here and nowhere else inside a capture: Finish waits for idle, so no
  // submitted batch can still be reading vertices from the previous one.
  g_inline_vertices.Reset();
}

constexpr uint32_t kMaxResolves = 96;
ResolveRecord g_resolves[kMaxResolves];
uint32_t g_resolve_count = 0;

void NoteRejectedConfig(const TargetConfig& cfg) {
  for (uint32_t i = 0; i < g_rejected_count; ++i) {
    if (g_rejected[i].cfg == cfg) {
      ++g_rejected[i].count;
      return;
    }
  }
  if (g_rejected_count < kMaxRejectedConfigs) {
    g_rejected[g_rejected_count].cfg = cfg;
    g_rejected[g_rejected_count].count = 1;
    ++g_rejected_count;
  }
}

// Defined below Finish, which is the only caller.
void ReadbackTargetToTga(D3D12Context& context, ID3D12Device* device, RenderTarget& target,
                         const std::filesystem::path& path, const char* label);

// CopyTextureRegion refuses a multisampled source outright -- the debug layer
// says "the destination resource multisampling properties must equal the source
// resource" -- and that one error kills the command list for the whole session:
// Close() fails, and every Reset() after it fails too, so no draw is ever
// recorded again. Measured with mcla_native_gfx_msaa=4: accepted=0 with
// fail_bind climbing without bound. Any readback of a pooled target therefore
// has to resolve into a single-sampled temporary first.
//
// Returns the resource to copy FROM, leaves it in COPY_SOURCE via `entry_state`,
// and parks the temporary in `keep_alive` so it outlives the caller's
// WaitForIdle. Returns nullptr when the temporary cannot be made -- copying the
// multisampled source anyway is what has to be avoided.
ID3D12Resource* ResolveForReadback(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                                   RenderTarget& target, D3D12_RESOURCE_STATES& entry_state,
                                   Microsoft::WRL::ComPtr<ID3D12Resource>& keep_alive) {
  ID3D12Resource* src = target.color.Get();
  if (!src || target.key.sample_count <= 1) {
    return src;
  }
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = target.key.width;
  d.Height = target.key.height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT(target.key.rt_format);
  d.SampleDesc.Count = 1;
  if (FAILED(context.device()->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
          D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&keep_alive)))) {
    return nullptr;
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = src;
  b.Transition.StateBefore = entry_state;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (entry_state != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
    cl->ResourceBarrier(1, &b);
  }
  cl->ResolveSubresource(keep_alive.Get(), 0, src, 0, DXGI_FORMAT(target.key.rt_format));
  if (entry_state != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b.Transition.StateAfter = entry_state;
    cl->ResourceBarrier(1, &b);
  }
  D3D12_RESOURCE_BARRIER t = {};
  t.Transition.pResource = keep_alive.Get();
  t.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
  t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &t);
  entry_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  return keep_alive.Get();
}

// Pooled targets were SINGLE-SAMPLED regardless of what the guest asked for.
// The reasoning was that MSAA only ever exists inside EDRAM on Xenos and the
// guest's own resolve lands a single-sampled image in main memory, so the
// intermediate could be simplified without changing the result. That is true
// of the RESULT and false of the IMAGE: the resolve is where the antialiasing
// happens, and skipping it is why the native path has no antialiasing at all.
// Measured: every one of the 2737 textures in "cap_city2.rdc" is msSamp=1,
// while the emulated capture of the same game has six at 2x and sixteen at 4x.
//
// The forced count applies to the HDR scene target only -- the one pass whose
// edges are visible, and the one whose resolves the pool knows how to route
// through a resolve step. The LDR composite is read back for presentation, and
// the aux passes (shadow 640x640, minimap 220x220) are left alone.
uint32_t PooledSampleCountFields(uint32_t rt_format, uint32_t ds_format, uint32_t width) {
  const uint32_t forced = REXCVAR_GET(mcla_native_gfx_msaa);
  if (forced != 2u && forced != 4u && forced != 8u) {
    return 1u;
  }
  if (rt_format != uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT) || ds_format == 0 || width < 1024u) {
    return 1u;
  }
  return forced;
}

uint32_t PooledSampleCount(const TargetConfig& cfg) {
  return PooledSampleCountFields(cfg.rt_format, cfg.ds_format, cfg.width);
}

RenderTargetKey PooledKey(const TargetConfig& cfg) {
  RenderTargetKey k;
  k.rt_format = cfg.rt_format;
  k.guest_msaa = cfg.guest_msaa;
  k.surface_pitch = cfg.surface_pitch;
  k.ds_format = cfg.ds_format;
  k.sample_count = PooledSampleCount(cfg);
  k.width = cfg.width;
  k.height = cfg.height;
  return k;
}

// One command list holds this many draws before being submitted. Large enough
// that submission overhead disappears, small enough that a batch stays well
// inside the TDR window and that the per-frame upload ring can back it.
constexpr uint32_t kDrawsPerBatch = 128;

// Returns the open command list, opening one if needed.
ID3D12GraphicsCommandList* EnsureFrame(D3D12Context& context) {
  if (g_cap.frame_open) {
    return context.CurrentCommandList();
  }
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl) {
    return nullptr;
  }
  g_cap.frame_open = true;
  g_cap.draws_in_batch = 0;
  context.ClearDebugMessages();
  return cl;
}

// Submits whatever has been recorded. Safe to call when nothing is open.
bool FlushBatch(D3D12Context& context) {
  if (!g_cap.frame_open) {
    return true;
  }
  g_cap.frame_open = false;
  g_cap.draws_in_batch = 0;
  return context.EndFrame();
}

// Resolves, reads back and writes the accumulated image plus the report.
void Finish(D3D12Context& context, PipelineCache& pipelines, BufferCache& buffers,
            RenderTargetPool& render_targets, const TextureCache::Stats& textures_stats,
            const TextureBinder::Stats& binder_stats) {
  g_cap.finished = true;
  // Whatever the last batch recorded still has to reach the GPU before the
  // target can be read back.
  FlushBatch(context);
  if (g_cap.renderdoc_capturing) {
    g_cap.renderdoc_capturing = false;
    RenderDocEndCapture(context.device());
  }
  ID3D12Device* device = context.device();
  // BOTH targets are written out, not one chosen between. The composite is the
  // final image in principle, but it only looks right once everything it
  // samples is bridged — while it is not, it reads back as a black screen and
  // the scene target is the only usable picture. Picking one meant a run could
  // produce no viewable output at all.
  const bool from_composite = g_cap.has_readback && !(g_cap.readback_config == g_cap.config);
  RenderTarget* composite = g_cap.has_readback ? render_targets.Find(g_cap.readback_key) : nullptr;
  RenderTarget* scene = g_cap.has_anchor ? render_targets.Acquire(context, g_cap.anchor_key, 0.0f)
                                         : nullptr;
  if (composite && from_composite && composite->color && scene && scene->color) {
    ReadbackTargetToTga(context, device, *scene, g_cap.dir / "mcla_native_gfx_scene.tga",
                        "anchor / scene pass");
  }
  RenderTarget* anchor = (composite && composite->color) ? composite : scene;
  if (!anchor || !anchor->color) {
    CLOGF("\nFAILED: no render target to read back\n");
    return;
  }
  CLOGF("\n  readback: %ux%u rt_format=%u  (%s)\n", anchor->key.width, anchor->key.height,
        anchor->key.rt_format,
        anchor == composite ? "composite pass" : "anchor / scene pass");
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl) {
    CLOGF("\nFAILED: BeginFrame for the readback\n");
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  Microsoft::WRL::ComPtr<ID3D12Resource> msaa_temp;
  // The source may be multisampled (mcla_native_gfx_msaa); ResolveForReadback
  // hands back a single-sampled stand-in when it is.
  // The state has to come from the target, not be assumed: the composite pass
  // is left in COPY_SOURCE by its own resolve, and declaring RENDER_TARGET here
  // would be a lie to the runtime.
  D3D12_RESOURCE_STATES entry_state = anchor->color_state;
  ID3D12Resource* src = ResolveForReadback(context, cl, *anchor, entry_state, msaa_temp);
  if (!src) {
    CLOGF("\nFAILED: could not resolve the multisampled anchor for readback\n");
    context.EndFrame();
    return;
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = src;
  b.Transition.StateBefore = entry_state;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (entry_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    cl->ResourceBarrier(1, &b);
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC src_desc = src->GetDesc();
  device->GetCopyableFootprints(&src_desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
  D3D12_RESOURCE_DESC rb = {};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = total;
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                  D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&readback));
  if (readback) {
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {}, src_loc = {};
    dst_loc.pResource = readback.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
  }
  if (entry_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = entry_state;
    cl->ResourceBarrier(1, &b);
  }

  const bool submitted = context.EndFrame();
  context.WaitForIdle();
  context.DrainDebugMessages("frame capture readback");

  CLOGF("\n=== RESULT ===\n  readback submit=%s\n", submitted ? "ok" : "FAILED");
  if (readback) {
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, size_t(total)};
    if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
      // Dimensions and format come from the target actually read back, which
      // is the composite pass, not the anchor: they differ in format (LDR
      // R8G8B8A8 vs the anchor's HDR R16G16B16A16_FLOAT) and decoding one as
      // the other would garble every pixel.
      const uint32_t rb_width = anchor->key.width;
      const uint32_t rb_height = anchor->key.height;
      const ImageCoverage cov = WriteReadbackTga(
          g_cap.dir / "mcla_native_gfx_frame.tga", static_cast<const uint8_t*>(mapped),
          fp.Footprint.RowPitch, rb_width, rb_height, anchor->key.rt_format, kClearColor);
      readback->Unmap(0, nullptr);
      const double total_px = double(g_cap.config.width) * g_cap.config.height;
      CLOGF("  wrote mcla_native_gfx_frame.tga (%ux%u)\n", g_cap.config.width,
            g_cap.config.height);
      CLOGF("  coverage: drawn=%llu (%.3f%%)  clear=%llu (%.3f%%)  untouched=%llu\n",
            (unsigned long long)cov.drawn, 100.0 * double(cov.drawn) / total_px,
            (unsigned long long)cov.cleared, 100.0 * double(cov.cleared) / total_px,
            (unsigned long long)cov.untouched);
      if (cov.has_bounds) {
        CLOGF("  bounding box: x %u..%u  y %u..%u\n", cov.min_x, cov.max_x, cov.min_y, cov.max_y);
      }
    }
  }

  const PipelineCache::Stats& ps = pipelines.stats();
  const BufferCache::Stats& bs = buffers.stats();
  CLOGF("\n  PSO: hits=%llu misses=%llu creation_failures=%llu\n", (unsigned long long)ps.hits,
        (unsigned long long)ps.misses, (unsigned long long)ps.creation_failures);
  CLOGF("  BufferCache: hits=%llu uploads=%llu reuploads=%llu merges=%llu "
        "upload_failures=%llu unreadable=%llu\n",
        (unsigned long long)bs.hits, (unsigned long long)bs.uploads,
        (unsigned long long)bs.reuploads, (unsigned long long)bs.merges,
        (unsigned long long)bs.upload_failures, (unsigned long long)bs.unreadable);

  CLOGF("\n  draws offered=%u accepted=%u\n", g_cap.offered, g_cap.accepted);
  CLOGF("  rejected: not_indexed=%u topology=%u no_shader=%u geometry=%u unsupplied_attr=%u "
        "other_target=%u shader_not_in_pack=%u\n",
        g_cap.rej_not_indexed, g_cap.rej_topology, g_cap.rej_no_shader, g_cap.rej_geometry,
        g_cap.rej_unsupplied, g_cap.rej_config, g_cap.rej_shader_missing);
  CLOGF("  failed while recording: bind=%u pso=%u constants=%u\n", g_cap.fail_bind,
        g_cap.fail_pso, g_cap.fail_constants);

  CLOGF("\n  TextureCache: hits=%llu uploads=%llu render_target_sourced=%llu "
        "unsupported_format=%llu decode_failures=%llu stale_gpu_addr=%llu\n",
        (unsigned long long)textures_stats.hits, (unsigned long long)textures_stats.uploads,
        (unsigned long long)textures_stats.render_target_hits,
        (unsigned long long)textures_stats.unsupported_format,
        (unsigned long long)textures_stats.decode_failures,
        (unsigned long long)textures_stats.stale_gpu_addresses);
  if (g_resolve_miss_count) {
    CLOGF("\n  resolve misses -- source pass never rendered (%u distinct):\n",
          g_resolve_miss_count);
    for (uint32_t i = 0; i < g_resolve_miss_count; ++i) {
      const ResolveMissRecord& r = g_resolve_misses[i];
      CLOGF("    dest=0x%08X %ux%u  <- src %ux%u  rt=%u ds=%u  %u resolves\n", r.address,
            r.dest_width, r.dest_height, r.src_width, r.src_height, r.rt_format,
            r.ds_format, r.count);
    }
  }
  CLOGF("  TextureBinder: srv_hits=%llu srv_misses=%llu sampler_hits=%llu sampler_misses=%llu "
        "unresolved=%llu\n",
        (unsigned long long)binder_stats.srv_hits, (unsigned long long)binder_stats.srv_misses,
        (unsigned long long)binder_stats.sampler_hits,
        (unsigned long long)binder_stats.sampler_misses,
        (unsigned long long)binder_stats.unresolved);
  CLOGF("  texture formats requested (%u distinct):\n", g_format_count);
  for (uint32_t i = 0; i < g_format_count; ++i) {
    CLOGF("    format %-3u resolved=%-7u unresolved=%u\n", g_formats[i].format,
          g_formats[i].resolved, g_formats[i].unresolved);
  }

  const RenderTargetPool::Stats& rts = render_targets.stats();
  // The anchor limit and the auxiliary cap are separate budgets. They used to
  // share one counter, so the shadow and reflection passes spent the scene's
  // draws and the visible draw distance collapsed as more passes were enabled.
  CLOGF("  draws: anchor=%u (limit %u)  auxiliary=%u\n", g_cap.accepted, g_cap.limit,
        g_cap.accepted_aux);
  CLOGF("  guest draw calls: indexed=%u  non_indexed=%u  BeginVertices=%u\n", g_guest_draws[0],
        g_guest_draws[1], g_guest_draws[2]);
  CLOGF("  inline draws: %u  vertices=%u  by primitive:", g_inline_draws, g_inline_verts);
  for (uint32_t k = 0; k < 16; ++k) {
    if (g_inline_by_prim[k]) {
      CLOGF(" %u=%u%s", k, g_inline_by_prim[k],
            k == 8 ? "(RECTLIST)" : (k == 13 ? "(QUADLIST)" : ""));
    }
  }
  CLOGF("\n");
  ReportShadowDraws();
  CLOGF("  topology expansion: %u draws -> %u indices  (%zu buffers cached)\n", g_expanded_draws,
        g_expanded_indices, g_topology.buffers_created());
  CLOGF("  topology rejections by primitive:");
  for (uint32_t k = 0; k < 16; ++k) {
    if (g_rejected_topology_by_prim[k]) {
      // xenos::PrimitiveType: 0x08 kRectangleList, 0x0D kQuadList, 0x0F
      // kPolygon. All three need index expansion; D3D12 has no topology.
      CLOGF(" %u=%u%s", k, g_rejected_topology_by_prim[k],
            k == 8 ? "(RECTLIST)" : (k == 13 ? "(QUADLIST)" : (k == 15 ? "(POLYGON)" : "")));
    }
  }
  CLOGF("\n");
  for (uint32_t k = 0; k < g_pass_tally_count; ++k) {
    const PassTally& q = g_pass_tallies[k];
    CLOGF("  [pass] %ux%u rt=%u  draws=%u  writing_colour=%u  mask_or=0x%X\n", q.width, q.height,
          q.rt_format, q.draws, q.draws_writing, q.mask_or);
  }
  CLOGF("\n  RenderTargetPool: targets=%llu resolves=%llu (depth %llu) registered=%llu "
        "lookup_hits=%llu lookup_misses=%llu (no_entry=%llu no_res=%llu small=%llu "
        "kind_mismatch=%llu)\n",
        (unsigned long long)rts.targets_created, (unsigned long long)rts.resolves,
        (unsigned long long)rts.resolves_depth,
        (unsigned long long)rts.resolve_copies_created, (unsigned long long)rts.lookup_hits,
        (unsigned long long)rts.lookup_misses, (unsigned long long)rts.miss_no_entry,
        (unsigned long long)rts.miss_no_resource, (unsigned long long)rts.miss_too_small,
        (unsigned long long)rts.miss_kind_mismatch);
  CLOGF("  resolves seen (%u distinct destinations):\n", g_resolve_count);
  for (uint32_t i = 0; i < g_resolve_count; ++i) {
    const ResolveRecord& r = g_resolves[i];
    CLOGF("    0x%08X %4ux%-4u source=%-6s %u resolves\n", r.address, r.width, r.height,
          r.from_depth ? "depth" : "colour", r.count);
  }

  CLOGF("  unresolved fetches (%u distinct addresses):\n", g_unresolved_count);
  for (uint32_t i = 0; i < g_unresolved_count; ++i) {
    const UnresolvedFetch& u = g_unresolved[i];
    CLOGF("    slot=%-2u 0x%08X %4ux%-4u format=%-3u tiled=%u  %u draws\n", u.slot, u.address,
          u.width, u.height, u.format, u.tiled ? 1u : 0u, u.count);
  }

  CLOGF("\n  other render target configurations seen (%u distinct):\n", g_rejected_count);
  for (uint32_t i = 0; i < g_rejected_count; ++i) {
    const RejectedConfig& r = g_rejected[i];
    CLOGF("    %4ux%-4u rt_format=%-3u ds_format=%-3u samples=%u  %u draws\n", r.cfg.width,
          r.cfg.height, r.cfg.rt_format, r.cfg.ds_format, r.cfg.sample_count, r.count);
  }
  CLOGF("\n  resolve destinations written out:\n");
  if (g_cap.dump_render_targets) {
    render_targets.DumpResolved(context, g_cap.dir, g_cap.log);
  }
  CLOGF("\n=== FRAME CAPTURE COMPLETED ===\n");

  REXLOG_INFO("[native_gfx] frame capture wrote mcla_native_gfx_frame.txt/.tga ({} draws)",
              g_cap.accepted);
  if (g_cap.log) {
    std::fclose(g_cap.log);
    g_cap.log = nullptr;
  }
}

}  // namespace

bool FrameCaptureDone() { return g_cap.finished; }

void ArmFrameCapture() {
  if (g_cap.started && !g_cap.finished) {
    // The frame we were recording just ended: close it on the next draw, which
    // already belongs to the following frame and must not be recorded.
    g_cap.end_requested = true;
    return;
  }
  g_cap.armed = true;
}

void SetFrameCaptureDumpTargets(bool dump) { g_cap.dump_render_targets = dump; }

void NoteFrameCaptureGuestDraw(int kind) {
  if (kind >= 0 && kind < 3) {
    ++g_guest_draws[kind];
  }
}

void NoteFrameCaptureResolve(uint32_t dest_address, uint32_t width, uint32_t height,
                             bool from_depth) {
  for (uint32_t i = 0; i < g_resolve_count; ++i) {
    if (g_resolves[i].address == dest_address && g_resolves[i].from_depth == from_depth) {
      ++g_resolves[i].count;
      return;
    }
  }
  if (g_resolve_count < kMaxResolves) {
    ResolveRecord& r = g_resolves[g_resolve_count++];
    r.address = dest_address;
    r.width = width;
    r.height = height;
    r.from_depth = from_depth;
    r.count = 1;
  }
}

void NoteFrameCaptureResolveMiss(uint32_t dest_address, uint32_t dest_width,
                                 uint32_t dest_height, uint32_t src_width,
                                 uint32_t src_height, uint32_t rt_format,
                                 uint32_t ds_format) {
  for (uint32_t i = 0; i < g_resolve_miss_count; ++i) {
    if (g_resolve_misses[i].address == dest_address &&
        g_resolve_misses[i].src_width == src_width &&
        g_resolve_misses[i].src_height == src_height) {
      ++g_resolve_misses[i].count;
      return;
    }
  }
  if (g_resolve_miss_count < kMaxResolveMisses) {
    g_resolve_misses[g_resolve_miss_count++] = ResolveMissRecord{
        dest_address, dest_width, dest_height, src_width,
        src_height,   rt_format,  ds_format,   1};
  }
}

void DumpResolveMisses() {
  // TEMP DIAG (remove after): the miss table was only ever reported through
  // CLOGF, which is a no-op in continuous mode -- so a resolve DROPPED for lack
  // of a natively-rendered source pass was recorded and never shown. That drop
  // (native_gfx.cpp, `RenderTargetPool::Find` returning null) is the prime
  // suspect for the composite input at 0x06ACD000, which the composite samples
  // as 1280x720 while the only resolve registered there is 64x32.
  static uint32_t tick = 0;
  static uint32_t last_count = 0;
  ++tick;
  // Only when the table has grown, and never more than once every 600 calls:
  // the set of missing passes is small and stops changing quickly.
  if (g_resolve_miss_count == last_count || (tick % 600) != 0) {
    return;
  }
  last_count = g_resolve_miss_count;
  FILE* f = std::fopen("native_gfx_diag.txt", "ab");
  if (!f) {
    return;
  }
  for (uint32_t i = 0; i < g_resolve_miss_count; ++i) {
    const ResolveMissRecord& r = g_resolve_misses[i];
    std::fprintf(f,
                 "RESOLVE_MISS dest=0x%08X dest=%ux%u src=%ux%u rt_fmt=%u ds_fmt=%u count=%u\n",
                 r.address, r.dest_width, r.dest_height, r.src_width, r.src_height, r.rt_format,
                 r.ds_format, r.count);
  }
  std::fflush(f);
  std::fclose(f);
}

namespace {
// Copies one render target's colour surface into a TGA. Used for both the
// composite and the scene target, so a run always leaves a viewable image even
// when one of them is not yet correct.
void ReadbackTargetToTga(D3D12Context& context, ID3D12Device* device, RenderTarget& target,
                                const std::filesystem::path& path, const char* label) {
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl || !target.color) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12Resource> msaa_temp;
  D3D12_RESOURCE_STATES entry_state = target.color_state;
  // Same rule as the Finish readback: a multisampled pooled target cannot be a
  // copy source, and trying kills the command list for good.
  ID3D12Resource* src = ResolveForReadback(context, cl, target, entry_state, msaa_temp);
  if (!src) {
    context.EndFrame();
    return;
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = src;
  b.Transition.StateBefore = entry_state;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (entry_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    cl->ResourceBarrier(1, &b);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC src_desc = src->GetDesc();
  device->GetCopyableFootprints(&src_desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
  D3D12_RESOURCE_DESC rb = {};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = total;
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                  D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&readback));
  if (readback) {
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {}, src_loc = {};
    dst_loc.pResource = readback.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
  }
  if (entry_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = entry_state;
    cl->ResourceBarrier(1, &b);
  }
  context.EndFrame();
  context.WaitForIdle();
  if (!readback) {
    return;
  }
  void* mapped = nullptr;
  const D3D12_RANGE range = {0, size_t(total)};
  if (FAILED(readback->Map(0, &range, &mapped))) {
    return;
  }
  const ImageCoverage cov =
      WriteReadbackTga(path, static_cast<const uint8_t*>(mapped), fp.Footprint.RowPitch,
                       target.key.width, target.key.height, target.key.rt_format, kClearColor);
  readback->Unmap(0, nullptr);
  const double total_px = double(target.key.width) * target.key.height;
  CLOGF("  extra readback (%s): %s  drawn=%llu (%.3f%%)\n", label,
        path.filename().string().c_str(), (unsigned long long)cov.drawn,
        100.0 * double(cov.drawn) / total_px);
}
}  // namespace

// Shared by the bound-stream and the inline-geometry entry points; they differ
// only in where the vertex data comes from, which is exactly what
// `inline_geom` carries into BuildGeometrySnapshot.
static void CaptureDrawImpl(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                            uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                            bool indexed, uint32_t draw_limit, D3D12Context& context,
                            ShaderDatabase& shaders, BufferCache& buffers, TextureCache& textures,
                            TextureBinder& binder, PipelineCache& pipelines,
                            RenderTargetPool& render_targets, uint32_t aux_stage,
                            const InlineGeometry* inline_geom) {
  if (g_cap.finished || draw_limit == 0) {
    return;
  }
  // Continuous mode ends a frame from the boundary hook (ResetContinuousFrame),
  // never from inside a draw, so the one-shot finish paths are skipped.
  if (g_cap.started && g_cap.end_requested && !g_cap.continuous) {
    Finish(context, pipelines, buffers, render_targets, textures.stats(), binder.stats());
    return;
  }
  ++g_cap.offered;
  // Visual bisection. g_cap.offered restarts every frame, so it is a stable
  // per-frame draw number: skip a half, look at the screen, halve again. Eleven
  // runs find one draw out of two thousand.
  //
  // This exists because guessing which draw makes an artefact has now failed
  // nine times in a row on the black shards, while every guess cost a build and
  // a run. Bisection cannot guess wrong.
  {
    const uint32_t skip_last = REXCVAR_GET(mcla_native_gfx_skip_draw_last);
    if (skip_last != 0) {
      const uint32_t skip_first = REXCVAR_GET(mcla_native_gfx_skip_draw_first);
      if (g_cap.offered >= skip_first && g_cap.offered <= skip_last) {
        ++g_cap.skipped_by_range;
        return;
      }
    }
  }

  if (element_count == 0) {
    ++g_cap.rej_not_indexed;
    return;
  }
  // A primitive D3D12 has no topology for is not automatically a lost draw:
  // quad lists, quad strips and polygons become triangle lists through a
  // generated index buffer. Measured before this existed: 265 kQuadList draws
  // per frame, all rejected here.
  bool expand_topology = false;
  if (PrimitiveTypeToTopology(primitive_type) == 0) {
    // Expansion renumbers vertices, so it only applies to a draw that has no
    // index buffer of its own; an indexed one would need its indices remapped
    // through the expansion. None were observed, so they stay rejected rather
    // than silently drawn wrong.
    if (!IsExpandableTopology(primitive_type) || indexed ||
        ExpandedIndexCount(primitive_type, element_count) == 0) {
      ++g_cap.rej_topology;
      if (primitive_type < 16) {
        ++g_rejected_topology_by_prim[primitive_type];
      }
      return;
    }
    expand_topology = true;
  }
  const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
  const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
  if (!vs_obj) {
    ++g_cap.rej_no_shader;
    return;
  }
  // A draw with no pixel shader is a DEPTH-ONLY pass, not a broken draw: the
  // shadow map is rendered that way, and rejecting it is why the shadow atlas
  // was never produced. D3D12 accepts a PSO with no PS.
  const bool depth_only = ps_obj == 0;

  const GuestRenderState rs = ReadRenderState(base, dev);
  const HostViewport hv = ComputeHostViewport(rs);
  TargetConfig cfg;
  cfg.rt_format = ColorRenderTargetFormatToDxgi(rs.color_format);
  if (REXCVAR_GET(mcla_native_gfx_surface_key)) {
    cfg.guest_msaa = rs.msaa_samples;
    cfg.surface_pitch = rs.surface_info & 0x3FFFu;
  }
  // Second colour target, only when RB_COLOR_MASK write-enables it. Left at 0
  // otherwise so single-target passes keep the pool key they already had.
  cfg.rt1_format = (rs.mrt && (REXCVAR_GET(mcla_native_gfx_mrt) & 0x2u))
                       ? ColorRenderTargetFormatToDxgi(rs.color1_format)
                       : 0u;
  cfg.ds_format = DepthRenderTargetFormatToDxgi(rs.depth_format);
  cfg.sample_count = SampleCountFromMsaa(rs.msaa_samples);
  cfg.width = uint32_t(hv.top_left_x + hv.width + 0.5f);
  cfg.height = uint32_t(hv.top_left_y + hv.height + 0.5f);
  // NOT sized from the scissor yet, deliberately. "brokenbuildings.rdc" shows
  // draws whose D3D12 viewport is 16384x16384 -- a guard-band viewport, where
  // the Xenos lets the viewport dwarf the surface and leaves the scissor to
  // clip -- landing in 320x4096, 160x4096 and 240x2736 targets, with 222 draws
  // of one frame going into the first instead of into the scene. But a
  // 16384-wide viewport starting at 0 would make `top_left + width` 16384 and
  // the check below would have rejected the draw, so the two numbers do not yet
  // add up and the missing piece is what the guest registers actually hold. The
  // VPORT line below prints exactly that; size from the scissor once it does.
  const bool guard_band_viewport =
      hv.width > float(kMaxTargetDimension) || hv.height > float(kMaxTargetDimension);
  if (cfg.rt_format == DXGI_FORMAT_UNKNOWN || cfg.width == 0 || cfg.height == 0 ||
      cfg.width > kMaxTargetDimension || cfg.height > kMaxTargetDimension) {
    ++g_cap.rej_config;
    ++g_cap.rej_cfg_target;
    return;
  }
  // TEMP DIAG (remove once the guard-band path is settled): every distinct
  // viewport/scissor shape, once each, so the registers behind this can be
  // checked against a real run rather than inferred from one capture.
  {
    static std::set<uint64_t> seen_vp_shape;
    static uint32_t lines = 0;
    const uint64_t sig = (uint64_t(uint32_t(hv.width)) << 40) ^
                         (uint64_t(uint32_t(hv.height)) << 20) ^
                         (uint64_t(cfg.width) << 10) ^ uint64_t(cfg.height);
    if (lines < 64 && seen_vp_shape.insert(sig).second) {
      ++lines;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "VPORT vp=(%.1f,%.1f %.1fx%.1f) vte=0x%08X scissor=%s(%d,%d..%d,%d) "
                     "-> target=%ux%u%s\n",
                     hv.top_left_x, hv.top_left_y, hv.width, hv.height, rs.vte_cntl,
                     rs.scissor_valid ? "ok" : "BAD", rs.scissor_left, rs.scissor_top,
                     rs.scissor_right, rs.scissor_bottom, cfg.width, cfg.height,
                     guard_band_viewport ? "  [guard band]" : "");
        std::fflush(f);
        std::fclose(f);
      }
    }
  }

  if (!g_cap.armed) {
    // Before the first frame boundary the frame is already half-consumed;
    // starting here would miss the earlier passes again.
    //
    // But `armed` also goes false mid-frame whenever Finish() runs, and in
    // continuous mode only the next frame boundary re-arms it. Every draw
    // after that point in the frame is dropped, silently until now.
    ++g_cap.rej_not_armed;
    return;
  }
  // Starting to record and choosing which pass to read back are separate
  // decisions. Tying them together meant the capture could not begin until a
  // main-scene draw appeared, so every pass running earlier in the frame --
  // the shadow map above all -- was rejected before it could render, and the
  // pool stayed at the anchor alone.
  if (!g_cap.started) {
    std::error_code ec;
    g_cap.dir = std::filesystem::current_path(ec);
    if (ec) {
      g_cap.dir = std::filesystem::path(".");
    }
    // Continuous mode never writes the report, so it never opens the log; a
    // null log makes every CLOGF a no-op, which is also what keeps the per-draw
    // fprintf/fflush out of the hot path at gameplay draw rates.
    if (!g_cap.continuous) {
      g_cap.log = std::fopen((g_cap.dir / "mcla_native_gfx_frame.txt").string().c_str(), "wb");
    }
    g_cap.limit = draw_limit;
    g_cap.started = true;
    ResetPerFrameTallies();
    // One-shot capture only. Its matching RenderDocEndCapture lives in Finish(),
    // which continuous mode never reaches -- and continuous mode clears
    // g_cap.started every frame, so this ran once per frame and never closed.
    // Measured: 177 StartFrameCapture and 0 EndFrameCapture in one session,
    // which leaves RenderDoc permanently inside a capture: no .rdc is written,
    // and its own capture key stops responding because it will not open a second
    // one. Continuous mode has its own properly paired trigger, in
    // NotifyFrameBoundary (native_gfx.cpp).
    if (!g_cap.continuous && RenderDocBeginCapture(context.device())) {
      g_cap.renderdoc_capturing = true;
    }
    CLOGF("=== FRAME CAPTURE ===\n\n");
    CLOGF("armed at a frame boundary; every pass renders, the anchor is read back\n\n");
    CLOGF("%-5s %-6s %-18s %-18s %-8s %-11s %s\n", "#", "elems", "vs_identity", "ps_identity",
          "prim", "target", "note");
  }

  // The anchor is the pass whose image is read back. Size alone does not
  // identify it: a 640x640 shadow/cube target clears any "big" bar. The scene
  // pass is the one shaped like the display, so require the aspect ratio too.
  if (!g_cap.has_anchor) {
    const float aspect = hv.height > 0.0f ? hv.width / hv.height : 0.0f;
    if (hv.width >= 1024.0f && aspect >= 1.5f && aspect <= 2.0f) {
      g_cap.config = cfg;
      g_cap.anchor_key = PooledKey(cfg);
      g_cap.has_anchor = true;
      CLOGF("anchor: %ux%u rt_format=%u ds_format=%u  (limit %u draws)\n\n", cfg.width,
            cfg.height, cfg.rt_format, cfg.ds_format, draw_limit);
    }
  }
  // Is this the display-shaped LDR composite/UI pass — the final frame we
  // present? Same predicate as the readback selection below. Among AUXILIARY
  // passes it is the composite; it needs its OWN draw budget so the shadow/effect
  // aux passes that run before it cannot starve it (which left the presented
  // composite flat, with no tonemap and no 2D/UI). The anchor (HDR scene) also
  // matches this shape but takes the cfg==config path above, so is_display only
  // ever selects the composite here.
  const float display_aspect = hv.height > 0.0f ? hv.width / hv.height : 0.0f;
  const bool is_display = hv.width >= 1024.0f && display_aspect >= 1.5f &&
                          display_aspect <= 2.0f && (rs.color_mask & 0xFu) != 0;
  if (!(g_cap.has_anchor && cfg == g_cap.config)) {
    // An auxiliary pass. Budgeted separately so the anchor cannot starve it,
    // and vice versa.
    // The shadow/effect aux passes share one budget; the composite/UI display
    // pass gets its OWN so it is never starved by them (see is_display above).
    // Measured: the 640x640 shadow pass alone offers ~1510 draws; the composite
    // /UI 1280x720 rt_format=28 pass runs LAST and used to get nothing — 680
    // draws rejected as `other_target`, no 2D in any image. The shared cap also
    // bounds how much work one frame piles onto the queue shared with the Xenia
    // command processor (at 8000 the capture failed to close its command list).
    //
    // The aux number is now mcla_native_gfx_auxcap (0 = uncapped) rather than a
    // constant. It was 2600, chosen by hand, and measurement showed it saturating
    // in dense scenes -- silently dropping the shadow/light draws it was meant to
    // bound. The cap stays (the documented failure is aggregate GPU load, which
    // resource accounting cannot remove) but its value now comes from the
    // measured peak in `peak_aux`, not from a guess.
    const uint32_t kMaxAuxDraws = uint32_t(REXCVAR_GET(mcla_native_gfx_auxcap));
    constexpr uint32_t kMaxCompositeDraws = 768;
    const uint32_t cap = is_display ? kMaxCompositeDraws
                                    : (kMaxAuxDraws ? kMaxAuxDraws : 0xFFFFFFFFu);
    const uint32_t used = is_display ? g_cap.accepted_composite : g_cap.accepted_aux;
    if (aux_stage == 0 || used >= cap) {
      ++g_cap.rej_config;
      ++g_cap.rej_cfg_budget;
      NoteRejectedConfig(cfg);
      return;
    }
    NoteRejectedConfig(cfg);
  }

  // Continuous-mode anchor draw cap. The DEVICE_HUNG TDR is aggregate GPU load
  // (~2000 anchor + ~2000 auxiliary native draws per frame ON TOP of Xenia, on a
  // weak GPU, exceeding the 2 s TDR limit), not one pathological draw. Capping
  // the anchor draws proves it (the hang stops) and is a direct load lever.
  // 0 = uncapped (default). Only the anchor pass is capped here; auxiliary
  // passes keep their own kMaxAuxDraws budget above.
  if (g_cap.continuous && g_cap.has_anchor && cfg == g_cap.config) {
    const uint32_t draw_cap = uint32_t(REXCVAR_GET(mcla_native_gfx_drawcap));
    if (draw_cap != 0 && g_cap.accepted >= draw_cap) {
      return;
    }
  }

  // Past the gate the draw gets recorded, so this pass has real content. The
  // last display-shaped pass THAT WRITES COLOUR is the image to read back.
  //
  // The colour-mask test is not a refinement, it is the whole condition. MCLA
  // runs a full-res depth prepass (1280x720, rt_format=28, 257 draws, colour
  // mask 0 on every one of them) AFTER the scene pass. Selecting by shape and
  // recency alone picked that prepass, whose target is by definition still the
  // clear colour: measured coverage drawn=0, clear=100%.
  {
    const float aspect = hv.height > 0.0f ? hv.width / hv.height : 0.0f;
    if (hv.width >= 1024.0f && aspect >= 1.5f && aspect <= 2.0f && (rs.color_mask & 0xFu) != 0) {
      // "Last display-shaped pass that writes colour" was enough while every
      // surface of that shape shared one pooled target. Once the key tells
      // them apart, several screen-sized targets exist at once -- the composite
      // the game displays, the UI surface the pause panel is drawn into, the
      // multisampled HDR scene -- and taking the last one is a coin toss.
      // Taking the UI surface presented the pause panel on a black field;
      // taking the HDR scene had the display slot build an R8G8B8A8 UAV and a
      // non-multisampled SRV over a multisampled R16G16B16A16_FLOAT resource,
      // and the device was removed with DXGI_ERROR_INVALID_CALL.
      //
      // What reaches the screen is always the single-sampled composite. Prefer
      // that; recency only decides among equals. With the surface key off,
      // guest_msaa is 0 everywhere and this is exactly the old rule.
      const bool single_sampled = cfg.guest_msaa == 0u;
      const bool have_single = g_cap.has_readback && g_cap.readback_config.guest_msaa == 0u;
      if (single_sampled || !have_single) {
        g_cap.readback_key = PooledKey(cfg);
        g_cap.readback_config = cfg;
        g_cap.has_readback = true;
      }
    }
  }

  const auto t_state = ProfileClock::now();
  const GeometrySnapshot geom =
      BuildGeometrySnapshot(base, dev, primitive_type, element_count, start_element, base_vertex,
                            indexed, shaders, nullptr, nullptr, nullptr, inline_geom);
  ProfileAdd(g_profile.state_us, t_state);
  if (!geom.complete || geom.streams.empty()) {
    ++g_cap.rej_geometry;
    return;
  }
  if (!geom.unsupplied.empty()) {
    // NO LONGER A REJECTION. The attribute is bound to a zero stream in
    // BuildGeometrySnapshot (geometry.cpp), matching what the Xenos does with an
    // unpatched vfetch. The counter stays because it is how the fix is measured:
    // rej_unsup(display) was 9243 per report before, and these draws are now
    // recorded instead of dropped.
    ++g_cap.rej_unsupplied;
    // TEMP DIAG (remove after): WHICH attribute, and whether the draw it kills
    // is display-shaped (i.e. part of the composite we present). The counter
    // alone cannot say whether this is eating the composite or only aux passes.
    if (is_display) {
      ++g_cap.rej_unsup_display;
    }
    {
      static std::set<uint64_t> seen_unsup;
      for (const UnsuppliedAttribute& u : geom.unsupplied) {
        const uint64_t sig = (uint64_t(u.semantic_name ? u.semantic_name[0] : '?') << 32) ^
                             (uint64_t(u.semantic_index) << 16) ^ (is_display ? 1u : 0u) ^
                             (uint64_t(geom.input_layout.size()) << 8);
        if (!seen_unsup.insert(sig).second) {
          continue;
        }
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "UNSUPPLIED %s%u vfetch_addr=%u display=%d supplied_elements=%zu\n",
                       u.semantic_name ? u.semantic_name : "?", u.semantic_index, u.vfetch_address,
                       is_display ? 1 : 0, geom.input_layout.size());
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    // Bisect switch for the zero stream itself.
    //
    // The comment above assumes an unpatched vfetch delivers zeros. That is an
    // assumption, not a measurement: on Xenos the D3D runtime patches the
    // shader's vfetch instructions from the vertex declaration, and an
    // unpatched one reads whatever fetch constant the microcode names -- which
    // may hold a stale but VALID binding, not zeros.
    //
    // It matters for POSITION1, which is one of the attributes measured
    // missing here. A shader blending POSITION0 with POSITION1 that gets zeros
    // drags vertices toward the origin, and long stretched triangles across
    // the scene is exactly what that looks like.
    //
    // Dropping the draw instead is not a fix -- it loses real geometry, which
    // is why the zero stream replaced it. It is a bisect: if an artefact
    // disappears with this on, the zero stream is producing it, and the fix
    // belongs in geometry.cpp. If the artefact stays, this whole branch is
    // eliminated and the cause is elsewhere.
    if (REXCVAR_GET(mcla_native_gfx_unsupplied_drop)) {
      return;
    }
    // Deliberately falls through: the zero stream makes the draw renderable.
  }

  const auto t_shader = ProfileClock::now();
  const ShaderUcodeRef vsr =
      ReadVertexShaderUcode(base, vs_obj, SelectVertexShaderVariant(base, vs_obj, ps_obj));
  const ShaderUcodeRef psr = depth_only ? ShaderUcodeRef{} : ReadPixelShaderUcode(base, ps_obj);
  if (!vsr.valid() || !IsGuestRangeReadable(vsr.guest_address, vsr.size_bytes)) {
    ++g_cap.rej_shader_missing;
    return;
  }
  if (!depth_only &&
      (!psr.valid() || !IsGuestRangeReadable(psr.guest_address, psr.size_bytes))) {
    ++g_cap.rej_shader_missing;
    return;
  }
  const uint8_t* const vs_ucode = reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), vsr.guest_address));
  const uint64_t vs_id = ShaderIdentity(vs_ucode, vsr.size_bytes);
  // Whether this shader derives its own vertex-fetch index rather than using
  // the one the hardware preloads into r0.x. Drives the vertex rebuild below.
  const bool vs_folds_fetch_index = HasComputedVertexFetchIndex(vs_ucode, vsr.size_bytes);
  const uint64_t ps_id =
      depth_only ? 0
                 : ShaderIdentity(reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
                                      const_cast<uint8_t*>(base), psr.guest_address)),
                                  psr.size_bytes);
  const uint32_t ps_spec = rs.alpha_test_enable ? 2u : 0u;
  // TEMP DIAG (remove after): MULTIPLE RENDER TARGETS.
  //
  // This runtime binds exactly one RTV -- OMSetRenderTargets(1, ...) here and
  // NumRenderTargets = 1 in pipeline_cache -- so a guest draw that writes oC1
  // silently loses it. RB_COLOR_MASK carries one write-enable nibble per
  // target, so any bit above 0xF says the guest asked for a second one.
  // xPropFoliage__PSGenerateImposterNight writes oC0 (impostor colour) AND oC1
  // (the packed normal the impostor shader later unpacks with *2-1), which is
  // exactly the atlas the flat canopy would come from.
  if ((rs.color_mask >> 4) != 0u) {
    static std::set<uint64_t> seen_mrt;
    const uint64_t sig = ps_id ^ (uint64_t(rs.color_mask) << 40);
    if (seen_mrt.size() < 64 && seen_mrt.insert(sig).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        // RB_COLOR1/2/3_INFO are 0x2003..0x2005, in the same shadow block as
        // RB_COLOR_INFO (0x2001 at dev+10372), so each is four bytes on.
        std::fprintf(f,
                     "MRT ps=%016llX mask=0x%08X color0=0x%08X color1=0x%08X color2=0x%08X "
                     "color3=0x%08X target=%ux%u\n",
                     (unsigned long long)ps_id, rs.color_mask, rs.color_info,
                     R32(base, dev + kDevRegColorInfo + 8), R32(base, dev + kDevRegColorInfo + 12),
                     R32(base, dev + kDevRegColorInfo + 16), cfg.width, cfg.height);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // The VS spec variant. Bit 0 is SPEC_CONSTANT_R11G11B10_NORMAL, which turns
  // tfetchR11G11B10() from a bit-preserving asfloat() pass-through into the
  // real unpack of a packed normal/tangent. Asking for 0 here -- which this did
  // -- always selected the pass-through, so every shader with a packed normal
  // ran asfloat() over a small integer and got a denormal: a zero normal on
  // 2148 of 2158 normal/tangent attributes in a frame. A shader that has no
  // packed normal ships only variant 0, and Lookup falls back to it.
  const uint32_t vs_spec = 1u;
  const ShaderBytecode vs_code = shaders.Lookup(vs_id, vs_spec, /*is_pixel=*/false);
  const ShaderBytecode ps_code =
      depth_only ? ShaderBytecode{} : shaders.Lookup(ps_id, ps_spec, /*is_pixel=*/true);
  ProfileAdd(g_profile.shader_us, t_shader);
  if (!vs_code.valid() || (!depth_only && !ps_code.valid())) {
    ++g_cap.rej_shader_missing;
    CLOGF("%-5u %-6u %016llX %016llX %-8u shader not in pack (vs=%d ps=%d)\n", g_cap.accepted,
          element_count, (unsigned long long)vs_id, (unsigned long long)ps_id, primitive_type,
          vs_code.valid() ? 1 : 0, ps_code.valid() ? 1 : 0);
    return;
  }

  ID3D12GraphicsCommandList* cl = EnsureFrame(context);
  if (!cl) {
    // The only fail_bind site that used to be silent, which made these
    // indistinguishable from a geometry failure in the tally.
    CLOGF("      no command list: %ux%u rt=%u prim=%u verts=%u\n", cfg.width, cfg.height,
          cfg.rt_format, primitive_type, element_count);
    {  // TEMP DIAG (BINDFAIL)
      static uint32_t n = 0;
      if (n++ < 24u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BINDFAIL site=no_command_list %ux%u\n", cfg.width, cfg.height);
          std::fclose(f);
        }
      }
    }
    ++g_cap.fail_bind;
    return;
  }

  // kRectangleList: build the four-corner vertices before the binding
  // snapshot, because the stream it resolves has to point at them and not at
  // the three the guest wrote. `geom` (guest-only) already carries the layout
  // needed to tell interpolatable float fields from packed ones.
  InlineGeometry inline_rect;
  if (inline_geom && primitive_type == 8 /* kRectangleList */) {
    const uint32_t out_verts = RectListVertexCount(element_count);
    const uint32_t out_bytes = out_verts * inline_geom->stride;
    uint64_t gpu = 0;
    uint8_t* dst = out_bytes ? g_inline_vertices.Allocate(context, out_bytes, &gpu) : nullptr;
    // Guard the guest read: a bad address here is an access violation inside
    // the game's own draw thread, with no D3D12 error to point at it.
    const bool src_sane = inline_geom->address >= 0x1000u &&
                          uint64_t(inline_geom->address) + element_count * inline_geom->stride <
                              0x100000000ull;
    const uint8_t* src = (dst && src_sane)
                             ? rex::memory::GuestPtr(const_cast<uint8_t*>(base),
                                                     inline_geom->address)
                             : nullptr;
    if (!dst || !src ||
        !SynthesiseRectList(src, element_count, inline_geom->stride, geom.input_layout, dst)) {
      ++g_cap.fail_bind;
      CLOGF("      rect synthesis failed: addr=0x%08X verts=%u stride=%u elements=%zu "
            "ring=%s guest=%s\n",
            inline_geom->address, element_count, inline_geom->stride, geom.input_layout.size(),
            dst ? "ok" : "FULL", src ? "ok" : "BAD");
      return;
    }
    inline_rect = *inline_geom;
    inline_rect.host_gpu_address = gpu;
    inline_rect.host_size_bytes = out_bytes;
    inline_geom = &inline_rect;
  }

  // Vertex/index uploads draw from the SAME per-slot upload ring as the
  // constants below, so they can exhaust it too. Watch the cache's own failure
  // counter across the call: on a ring exhaustion the batch has to be submitted
  // or the frame livelocks exactly as described at the constant upload.
  const uint64_t buffer_upload_failures_before = buffers.stats().upload_failures;
  const auto t_geom = ProfileClock::now();
  const GeometrySnapshot bound =
      BuildGeometrySnapshot(base, dev, primitive_type, element_count, start_element, base_vertex,
                            indexed, shaders, &buffers, &context, cl, inline_geom);
  ProfileAdd(g_profile.geom_us, t_geom);
  // TEMP DIAG (remove after): every draw into the 220x220 minimap target, with
  // the vertex layout it actually got. The circular punch there is
  // xAlphaModulate__PS_Textured, which samples the mask at iTexCoord0.xy
  // clamped to [0.05, 0.95] and outputs 1 - mask for a One/One/ReversedSubtract
  // alpha blend. If TEXCOORD0 arrives with only .x supplied -- the exact defect
  // that made the menus black earlier -- the .y is 0, the fetch reads a thin
  // strip at the top of the mask instead of the circle, and a mask of 1 makes
  // the subtract erase nothing: the map keeps its square corners.
  if (cfg.width == 220 && cfg.height == 220) {
    static std::set<uint64_t> seen_mm;
    uint64_t sig = uint64_t(bound.input_layout.size());
    for (const InputElement& e : bound.input_layout) {
      sig = sig * 1099511628211ull ^ (uint64_t(e.dxgi_format) << 8) ^ uint64_t(e.semantic_index) ^
            uint64_t(e.semantic_name ? e.semantic_name[0] : 0);
    }
    if (seen_mm.insert(sig).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "MINIMAP layout (%zu elements):", bound.input_layout.size());
        for (const InputElement& e : bound.input_layout) {
          std::fprintf(f, " %s%u=fmt%u@slot%u+%u", e.semantic_name ? e.semantic_name : "?",
                       e.semantic_index, e.dxgi_format, e.input_slot, e.aligned_byte_offset);
        }
        std::fprintf(f, " | unsupplied=%zu\n", bound.unsupplied.size());
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  if (buffers.stats().upload_failures != buffer_upload_failures_before) {
    ++g_cap.ring_flushes;
    FlushBatch(context);
    {  // TEMP DIAG (BINDFAIL)
      static uint32_t n = 0;
      if (n++ < 24u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BINDFAIL site=upload_failure %ux%u\n", cfg.width, cfg.height);
          std::fclose(f);
        }
      }
    }
    ++g_cap.fail_bind;
    return;
  }
  if (!bound.complete) {
    {  // TEMP DIAG (BINDFAIL)
      static uint32_t n = 0;
      if (n++ < 24u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BINDFAIL site=geometry %ux%u reason=%s\n", cfg.width, cfg.height, bound.failure ? bound.failure : "?");
          std::fclose(f);
        }
      }
    }
    ++g_cap.fail_bind;
    CLOGF("%-5u %-6u %016llX %016llX %-8u geometry: %s\n", g_cap.accepted, element_count,
          (unsigned long long)vs_id, (unsigned long long)ps_id, primitive_type,
          bound.failure ? bound.failure : "?");
    return;  // the batch stays open; this draw simply contributes nothing
  }

  // Scratch, not per-draw allocations: this runs thousands of times a frame on
  // one thread, and three 4 KiB vectors per draw is three malloc/free pairs per
  // draw for buffers whose size never changes.
  static std::vector<uint8_t> vs_bank(kAluBankBytes), ps_bank(kAluBankBytes);
  ReadConstantBank(base, dev + kDevVsConstantBankOffset, vs_bank.data());
  ReadConstantBank(base, dev + kDevPsConstantBankOffset, ps_bank.data());
  // Right after the read, so the mirror below stores the same bytes that were
  // uploaded and its comparison stays meaningful.
  ApplyColorExpBias(vs_bank.data(), base, dev);
  ApplyColorExpBias(ps_bank.data(), base, dev);

  // Resolve destinations are created and copied HERE, before the textures are
  // bound. NoteResolve inserts the resolved_ entry immediately but builds the
  // resource lazily, and the flush used to run only at render setup (after
  // BindAll). So a pass that samples a resolve produced earlier in the SAME
  // frame — the exposure/luminance reduction chain does exactly this, each
  // stage reading the previous — found the entry present but its resource null,
  // fell back to the 1x1 white texture, and the chain collapsed to zero. That
  // zero exposure multiplied the whole scene to black in the tonemap. Flushing
  // before BindAll makes the resource exist and hold real data when its SRV is
  // created; the render-target state is still fixed by the flush at draw setup.
  render_targets.FlushPendingCopies(context, cl);

  static std::vector<uint8_t> shared(kSharedConstantsBytes);
  std::memset(shared.data(), 0, kSharedConstantsBytes);
  BoundTexture bound_tex[32];
  uint32_t bound_tex_count = 0;
  const auto t_bind = ProfileClock::now();
  // Everything the pool can do that swaps the resource behind a guest address.
  // Contents changing is deliberately absent: the SRV still names the same
  // resource and the GPU sees the new data without any rebind.
  const RenderTargetPool::Stats& rts = render_targets.stats();
  const uint64_t rt_guard = rts.targets_created + rts.resolves + rts.resolves_depth +
                            rts.resolve_copies_created + rts.resolves_without_target;
  binder.BindAll(context, cl, base, dev, textures, shared.data(), bound_tex, &bound_tex_count, 32,
                 rt_guard);
  ProfileAdd(g_profile.bind_us, t_bind);
  {  // TEMP DIAG (EYE): the eye/teeth material.
    //
    // The character .xrsc names its materials: drv_mp_01_set has ten grmShader
    // blocks and exactly one is `Character_eyes_normalmap`, whose base shader is
    // `xCharacter_teeth_normalmap`. So the eye is its OWN material with its own
    // pixel shader, and the identities below are that shader's -- PS is the lit
    // pass, PS_ShadowBlend the one the night path uses.
    //
    // What it prints, and why each field is here: `tint` is c101, which this
    // shader (and only five others in the whole pack) multiplies straight into
    // the diffuse -- `r6.yzw = r6.yzw * tintColor.xyz` -- so a zero there paints
    // the eye black on its own. The texture list is next because the same line
    // goes black if the diffuse fetch resolves to nothing.
    // ANISOCENSUS: why anisotropic filtering has no visible effect.
    //
    // Anisotropy only exists to pick a FINER mip level along the major axis. A
    // texture with a single level has nothing to pick, so a sampler can be a
    // perfectly formed D3D12_FILTER_ANISOTROPIC with MaxAnisotropy 16 and still
    // change no pixel. Earlier notes measured 150 of 300 binds >= 512x512 with
    // mip_max_level == 0 and the other 150 on kBaseMap, i.e. ZERO eligible with
    // a chain -- but that was before the packed-mip-tail fix, so it is re-measured
    // here rather than trusted.
    //
    // Eligibility is the same rule the sampler builder uses: not base-map
    // (mip_filter != 2) and both min and mag linear.
    if (REXCVAR_GET(mcla_native_gfx_aniso_census)) {
      static uint64_t big = 0, big_elig = 0, big_elig_mips = 0, big_basemap = 0, big_nomips = 0;
      static uint64_t big_basemap_mips = 0;
      static uint64_t big_basemap_fmt[64] = {};
      static uint64_t all = 0, all_elig = 0, all_elig_mips = 0;
      static uint64_t draws_seen = 0;
      for (uint32_t i = 0; i < bound_tex_count; ++i) {
        const BoundTexture& b = bound_tex[i];
        if (!b.fetch.type_valid || !b.fetch.width) {
          continue;
        }
        const bool eligible = b.sampler.mip_filter != 2u && b.sampler.min_filter == 1u &&
                              b.sampler.mag_filter == 1u;
        const bool has_mips = b.fetch.mip_address != 0u && b.fetch.mip_max_level != 0u;
        ++all;
        if (eligible) {
          ++all_elig;
          if (has_mips) ++all_elig_mips;
        }
        if (b.fetch.width >= 512u && b.fetch.height >= 512u) {
          ++big;
          if (b.sampler.mip_filter == 2u) {
            ++big_basemap;
            // Os base-map sao a maior fatia dos binds grandes e sao justamente
            // os que ficam de fora da anisotropia. Se ELES tiverem cadeia de
            // mip, um override estilo driver (forcar aniso e soltar o MaxLOD)
            // teria o que filtrar; se nao tiverem, nao tem.
            if (has_mips) ++big_basemap_mips;
            // Histograma de formato dessa populacao: decide se gerar mip no host
            // e um box filter trivial (formato sem compressao) ou exige
            // decodificar e recomprimir BC.
            if (b.fetch.format < 64u) ++big_basemap_fmt[b.fetch.format];
          }
          if (!has_mips) ++big_nomips;
          if (eligible) {
            ++big_elig;
            if (has_mips) ++big_elig_mips;
          }
        }
      }
      if ((++draws_seen % 20000ull) == 0ull) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f,
                       "ANISOCENSUS binds=%llu elegiveis=%llu elegiveis_com_mip=%llu | "
                       ">=512: %llu basemap=%llu (com_mip=%llu) sem_mip=%llu elegiveis=%llu "
                       "elegiveis_com_mip=%llu\n",
                       (unsigned long long)all, (unsigned long long)all_elig,
                       (unsigned long long)all_elig_mips, (unsigned long long)big,
                       (unsigned long long)big_basemap, (unsigned long long)big_basemap_mips,
                       (unsigned long long)big_nomips,
                       (unsigned long long)big_elig, (unsigned long long)big_elig_mips);
          std::fprintf(f, "ANISOFMT basemap>=512 por formato:");
          for (uint32_t k = 0; k < 64u; ++k) {
            if (big_basemap_fmt[k]) {
              std::fprintf(f, " f%u=%llu", k, (unsigned long long)big_basemap_fmt[k]);
            }
          }
          std::fprintf(f, "\n");
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    // Census kept because it is what FOUND the identity: the number in
    // shader_report.tsv's identity column is NOT the runtime's key. The pack key
    // is the one build_shader_pack.collect_shaders() returns, and for this shader
    // the two differ (report 6F13A15EB1C89968, pack 5E41952680F1F209). The census
    // prints every pixel shader the scene runs with a draw count, and the names
    // come from the pack map offline.
    if (const uint32_t census_every = REXCVAR_GET(mcla_native_gfx_ps_census)) {
      constexpr uint32_t kCensus = 256;
      struct CensusRow {
        uint64_t ps = 0;
        uint64_t vs = 0;
        uint32_t count = 0;
        uint32_t color0_format = 0;  // DXGI of the declaration's COLOR0, 0 = none
        uint32_t color0_offset = 0;
        uint32_t tex_addr[3] = {0, 0, 0};
        uint32_t tex_w[3] = {0, 0, 0};
        uint32_t tex_h[3] = {0, 0, 0};
        uint32_t tex_fmt[3] = {0, 0, 0};
        uint32_t tex_src[3] = {0, 0, 0};
      };
      static CensusRow rows[kCensus];
      static uint64_t draws_seen = 0;
      const uint32_t start_slot = uint32_t((ps_id ^ (ps_id >> 32)) % kCensus);
      for (uint32_t probe = 0; probe < kCensus; ++probe) {
        CensusRow& r = rows[(start_slot + probe) % kCensus];
        if (r.count && r.ps != ps_id) {
          continue;
        }
        if (!r.count) {
          // First sighting: record everything that identifies the material, so
          // ONE run answers "which shader is this and what does it read".
          r.ps = ps_id;
          r.vs = vs_id;
          for (const auto& el : bound.input_layout) {
            if (el.semantic_name && el.semantic_index == 0 &&
                std::strcmp(el.semantic_name, "COLOR") == 0) {
              r.color0_format = el.dxgi_format;
              r.color0_offset = el.aligned_byte_offset;
              break;
            }
          }
          for (uint32_t i = 0; i < bound_tex_count && i < 3u; ++i) {
            r.tex_addr[i] = bound_tex[i].fetch.base_address;
            r.tex_w[i] = bound_tex[i].fetch.width;
            r.tex_h[i] = bound_tex[i].fetch.height;
            r.tex_fmt[i] = bound_tex[i].fetch.format;
            r.tex_src[i] = uint32_t(bound_tex[i].source);
          }
        }
        ++r.count;
        break;
      }
      if ((++draws_seen % uint64_t(census_every)) == 0ull) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "PSCENSUS after %llu draws\n", (unsigned long long)draws_seen);
          for (const CensusRow& r : rows) {
            if (!r.count) {
              continue;
            }
            std::fprintf(f, "  PS %016llX vs %016llX n=%u col0=fmt%u@%u tex",
                         (unsigned long long)r.ps, (unsigned long long)r.vs, r.count,
                         r.color0_format, r.color0_offset);
            for (uint32_t i = 0; i < 3u; ++i) {
              if (r.tex_addr[i]) {
                std::fprintf(f, " 0x%08X/%ux%u/f%u/s%u", r.tex_addr[i], r.tex_w[i], r.tex_h[i],
                             r.tex_fmt[i], r.tex_src[i]);
              }
            }
            std::fprintf(f, "\n");
          }
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    {  // TEMP DIAG (RIM): where the wheel's paint colour comes from.
      //
      // xRimMain__VS_Common does not read a paint constant directly. It reads
      // the tint INDEX out of the vertex:
      //
      //   r0.yzw = tfetchTexcoord(g_SwappedTexcoords, iTexCoord0, 0).xyz;
      //   ps = trunc(r0.w);                       // TEXCOORD0's third component
      //   a0 = (int)clamp(floor(ps + 0.5), -256, 255);
      //   oTexCoord4.xy = tintColors(0 + a0).xy;  // = VS constant 206 + a0
      //
      // So SPOKE A, SPOKE B and LIP are the same shader picking different
      // `tintColors` slots, and the slot number is baked into TEXCOORD0.z of
      // each vertex. A wrong .z -- a declaration one component short, or the
      // texcoord swap applied to the wrong stream -- reads a NEIGHBOURING part's
      // colour, which is exactly "I set black and got blue".
      //
      // Prints the declaration, the first vertices' TEXCOORD0, and the tint
      // registers, so the index the mesh asks for can be compared against the
      // colour that sits there.
      static uint32_t rim_taken = 0;
      if (vs_id == 0x0824D18D088E17E8ull && rim_taken < REXCVAR_GET(mcla_native_gfx_rim_probe)) {
        ++rim_taken;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "RIM#%u vs=%016llX ps=%016llX swapped_tc=%u elems=%u\n", rim_taken,
                       (unsigned long long)vs_id, (unsigned long long)ps_id,
                       REXCVAR_GET(mcla_native_gfx_swapped_texcoords) ? 1u : 0u,
                       uint32_t(bound.input_layout.size()));
          uint32_t tc0_off = 0xFFFFFFFFu, tc0_fmt = 0, tc0_slot = 0;
          for (const auto& el : bound.input_layout) {
            if (el.semantic_name && el.semantic_index == 0 &&
                std::strcmp(el.semantic_name, "TEXCOORD") == 0) {
              tc0_off = el.aligned_byte_offset;
              tc0_fmt = el.dxgi_format;
              tc0_slot = el.input_slot;
            }
            std::fprintf(f, "  RIMDECL %s%u stream=%u off=%u dxgi=%u\n", el.semantic_name,
                         el.semantic_index, el.input_slot, el.aligned_byte_offset,
                         el.dxgi_format);
          }
          // The tint table: VS constant registers 206..215 = tintColors(0..9).
          for (uint32_t i = 0; i < 10u; ++i) {
            float v[4];
            const uint32_t ea = dev + kDevVsConstantBankOffset + (206u + i) * 16u;
            for (uint32_t c = 0; c < 4; ++c) {
              const uint32_t bits = R32(base, ea + c * 4u);
              std::memcpy(&v[c], &bits, 4);
            }
            std::fprintf(f, "  RIMTINT[%u] c%u %.4f %.4f %.4f %.4f\n", i, 206u + i, v[0], v[1],
                         v[2], v[3]);
          }
          // And TEXCOORD0 of the first vertices, straight out of the bound
          // stream. The fetch base of a bound stream is PHYSICAL.
          const VertexStream* st = nullptr;
          for (const auto& vst : bound.streams) {
            if (vst.fetch_slot == FetchSlotForStream(tc0_slot)) { st = &vst; break; }
          }
          if (!st && tc0_slot < bound.streams.size()) st = &bound.streams[tc0_slot];
          if (tc0_off != 0xFFFFFFFFu && st && st->guest_base && st->stride) {
            const uint8_t* phys = TranslatePhysicalGuest(st->guest_base);
            const uint64_t need = uint64_t(st->stride) * 4ull;
            if (phys && IsPhysicalRangeReadable(st->guest_base, need)) {
              for (uint32_t vtx = 0; vtx < 4u; ++vtx) {
                uint32_t d[4] = {0, 0, 0, 0};
                for (uint32_t c = 0; c < 4u; ++c) {
                  if (tc0_off + c * 4u + 4u > st->stride) break;
                  std::memcpy(&d[c], phys + vtx * st->stride + tc0_off + c * 4u, 4);
                  d[c] = __builtin_bswap32(d[c]);
                }
                float fv[4];
                for (uint32_t c = 0; c < 4u; ++c) std::memcpy(&fv[c], &d[c], 4);
                (void)fv;
                // TEXCOORD0 here is R16G16B16A16_FLOAT: four halfs, big-endian
                // in guest memory. Decoding it as two byte-swapped dwords
                // scrambles the pairs, so read the halfs one at a time.
                float h[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (uint32_t c = 0; c < 4u; ++c) {
                  if (tc0_off + c * 2u + 2u > st->stride) break;
                  const uint8_t* q = phys + vtx * st->stride + tc0_off + c * 2u;
                  const uint16_t bits = uint16_t((uint16_t(q[0]) << 8) | q[1]);
                  h[c] = RimHalfToFloat(bits);
                }
                // tfetchTexcoord swaps to .yxwz when the semantic's bit is set,
                // and the shader takes its tint index from the THIRD component
                // of the result -- so the swap decides between the vertex's own
                // .z and its .w. Print the slot each choice lands on.
                const int a0_swapped = int(std::floor(h[3] + 0.5f));
                const int a0_plain = int(std::floor(h[2] + 0.5f));
                std::fprintf(f,
                             "  RIMVTX%u raw %08X %08X  tc0=%.3f %.3f %.3f %.3f "
                             "a0_swapped=%d a0_plain=%d\n",
                             vtx, d[0], d[1], h[0], h[1], h[2], h[3], a0_swapped, a0_plain);
              }
            } else {
              std::fprintf(f, "  RIMVTX unreadable base=0x%08X stride=%u\n", st->guest_base,
                           st->stride);
            }
          }
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    static uint32_t taken = 0;
    const bool is_eye = ps_id == 0x5E41952680F1F209ull ||   // __PS, the lit pass
                        ps_id == 0xD7BA99C168F84D73ull;    // __PS_ShadowBlend
    if (is_eye && taken < REXCVAR_GET(mcla_native_gfx_eye_probe)) {
      ++taken;
      auto psreg = [&](uint32_t reg, float* out) {
        const uint32_t ea = dev + kDevPsConstantBankOffset + reg * 16u;
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t bits = R32(base, ea + i * 4u);
          std::memcpy(&out[i], &bits, 4);
        }
      };
      float tint[4];
      psreg(101, tint);
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "EYE#%u ps=%016llX vs=%016llX tint=%.4f,%.4f,%.4f,%.4f "
                     "alpha_test=%d blend0=%08X tex=%u\n",
                     taken, (unsigned long long)ps_id, (unsigned long long)vs_id, tint[0], tint[1],
                     tint[2], tint[3], rs.alpha_test_enable ? 1 : 0, rs.blend_control0,
                     bound_tex_count);
        for (uint32_t i = 0; i < bound_tex_count; ++i) {
          const BoundTexture& b = bound_tex[i];
          std::fprintf(f,
                       "  EYETEX slot=%u addr=0x%08X %ux%u fmt=%u gamma=%d swz=0x%03X "
                       "resolved=%d source=%u srv=%u\n",
                       b.fetch_slot, b.fetch.base_address, b.fetch.width, b.fetch.height,
                       b.fetch.format, b.fetch.gamma ? 1 : 0, b.fetch.swizzle,
                       b.resolved ? 1 : 0, uint32_t(b.source), b.srv_descriptor_index);
          // Slot 13 is the shadow collector, and the whole night path of this
          // shader is gated on one sample of it: world XZ / 256 + 0.5, then
          // `saturate(-0.25 + s)` scales the light. A zero there kills the eye
          // outright. Print what guest memory actually holds -- the fetch
          // address is PHYSICAL, so it needs the physical translation, not R32.
          if (b.fetch_slot == 13 && b.fetch.width && b.fetch.height) {
            const uint64_t bytes = uint64_t(b.fetch.width) * b.fetch.height * 4ull;
            const uint8_t* phys = TranslatePhysicalGuest(b.fetch.base_address);
            if (phys && IsPhysicalRangeReadable(b.fetch.base_address, bytes)) {
              uint32_t lo = 0xFFFFFFFFu, hi = 0, first[4] = {0, 0, 0, 0};
              const uint32_t n = uint32_t(bytes / 4ull);
              for (uint32_t t = 0; t < n; ++t) {
                uint32_t d = 0;
                std::memcpy(&d, phys + t * 4u, 4);
                d = ((d & 0xFFu) << 24) | ((d & 0xFF00u) << 8) | ((d >> 8) & 0xFF00u) |
                    ((d >> 24) & 0xFFu);
                if (d < lo) lo = d;
                if (d > hi) hi = d;
                if (t < 4) first[t] = d;
              }
              std::fprintf(f,
                           "  EYECOLL guest %u texels min=%08X max=%08X first=%08X %08X %08X "
                           "%08X\n",
                           n, lo, hi, first[0], first[1], first[2], first[3]);
            } else {
              std::fprintf(f, "  EYECOLL guest UNREADABLE at 0x%08X\n", b.fetch.base_address);
            }
            // And the other half of the question: does the POOL know this
            // address? `gpu_produced` = a host target exists at exactly this
            // address and extent; `stale` = the address is a known resolve
            // destination but the extent differs, which is the keying failure
            // that would make the binder fall back to decoding guest memory.
            D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
            const bool has_res = render_targets.FindResolvedTarget(
                                     b.fetch.base_address, b.fetch.width, b.fetch.height,
                                     /*want_depth=*/false, &st) != nullptr;
            std::fprintf(f, "  EYECOLL pool gpu_produced=%d stale=%d resolved_target=%d\n",
                         render_targets.IsGpuProduced(b.fetch.base_address, b.fetch.width,
                                                      b.fetch.height)
                             ? 1
                             : 0,
                         render_targets.IsStaleGpuAddress(b.fetch.base_address, b.fetch.width,
                                                          b.fetch.height)
                             ? 1
                             : 0,
                         has_res ? 1 : 0);
          }
        }
        // Every constant this shader declares, so a day run and a night run can be
        // diffed register for register. c63 ShaderGlobals is the one the shader
        // branches on (`ShaderGlobals.z >= 8.0`), which is why it is here even
        // though nothing else reads it.
        static const struct { uint32_t reg; const char* name; } kRegs[] = {
            {26, "gLightAmbient"}, {27, "gInvColorExpBias"}, {39, "gLightColor0"},
            {40, "gLightColor1"},  {63, "ShaderGlobals"},    {100, "normalMapMod"},
            {102, "diffuseMod"},   {103, "envMod"},          {104, "metallic"},
            {105, "fresnelExp"},   {106, "fresnelMin"},      {107, "fresnelMax"},
            {108, "SpecExp"},      {109, "SpecIntensity"},   {110, "wrapAmount"},
        };
        for (const auto& r : kRegs) {
          float v[4];
          psreg(r.reg, v);
          std::fprintf(f, "  EYEC c%-3u %-17s %.4f %.4f %.4f %.4f\n", r.reg, r.name, v[0], v[1],
                       v[2], v[3]);
        }
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // TEMP DIAG (MESHCHK): does GUEST memory, right now, at this draw's fetch
  // address, decode as the mesh the draw declares?
  //
  // corruptnpc.rdc has two xPed draws whose bound bytes are not ped vertices at
  // ANY stride or alignment -- the packed NORMAL at +20 is a unit vector in 100%
  // of a healthy ped's vertices and in 37% (chance) of theirs. That splits three
  // ways and the capture cannot tell them apart:
  //
  //   guest bytes ARE a valid mesh  -> we uploaded stale bytes; BufferCache lost
  //                                    an invalidation
  //   guest bytes are garbage too   -> the fetch address is wrong, or the mesh
  //                                    is simply not loaded yet; the cache is
  //                                    innocent and the hunt moves
  //
  // Read-only, deduplicated by fetch address, and it never touches the GPU --
  // the point is precisely to compare against what the GPU was given.
  if (!inline_geom && !bound.streams.empty()) {
    // The declaration's own NORMAL slot. Hardcoding +20 would only be right for
    // xPed; asking the layout makes this work for every skinned draw.
    uint32_t norm_off = 0xFFFFFFFFu, norm_slot = 0;
    bool skinned = false;
    for (const auto& el : bound.input_layout) {
      if (!el.semantic_name || el.semantic_index != 0) continue;
      if (std::strcmp(el.semantic_name, "NORMAL") == 0 && el.dxgi_format == 42u) {
        norm_off = el.aligned_byte_offset;
        norm_slot = el.input_slot;
      } else if (std::strcmp(el.semantic_name, "BLENDINDICES") == 0) {
        skinned = true;
      }
    }
    // SKINNED only. The first pass filled a 256-entry table with world geometry
    // (all 100%) before a single pedestrian was drawn, so the one draw family
    // this exists for never got a slot.
    if (!skinned) norm_off = 0xFFFFFFFFu;
    const VertexStream* st = nullptr;
    for (const auto& s : bound.streams) {
      if (s.fetch_slot == FetchSlotForStream(norm_slot)) { st = &s; break; }
    }
    if (!st && norm_slot < bound.streams.size()) st = &bound.streams[norm_slot];
    if (norm_off != 0xFFFFFFFFu && st && st->stride >= norm_off + 4u && st->guest_base &&
        st->guest_size >= st->stride) {
      // Deduplicated only AFTER the verdict, and only for the failures, so a
      // healthy mesh never spends a slot and the table cannot fill before the
      // corrupt pedestrian walks into frame.
      static uint32_t seen_bad[512];
      static uint32_t seen_bad_n = 0;
      static uint64_t n_ok = 0, n_bad = 0, n_unreadable = 0, n_stale = 0;
      {
        const uint32_t nv = std::min<uint32_t>(st->guest_size / st->stride, 256u);
        // The stream base out of a fetch constant is PHYSICAL; reading it
        // through the virtual membase lands on unmapped pages.
        const uint8_t* p = TranslatePhysicalGuest(st->guest_base);
        const bool readable =
            p != nullptr && IsPhysicalRangeReadable(st->guest_base, uint64_t(nv) * st->stride);
        uint32_t unit = 0;
        if (readable) {
          for (uint32_t v = 0; v < nv; ++v) {
            uint32_t d;
            std::memcpy(&d, p + v * st->stride + norm_off, 4);
            d = __builtin_bswap32(d);  // guest vertex data is big-endian
            // k_2_10_10_10, three signed 10-bit components over 511.
            const auto comp = [](uint32_t raw) {
              const int32_t c = int32_t(raw & 0x3FFu);
              return float(c & 0x200 ? c - 1024 : c) / 511.0f;
            };
            const float x = comp(d), y = comp(d >> 10), z = comp(d >> 20);
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len >= 0.85f && len <= 1.15f) ++unit;
          }
        }
        const uint32_t pct = nv ? unit * 100u / nv : 0u;
        if (!readable) {
          ++n_unreadable;
        } else if (pct >= 60u) {
          ++n_ok;
        } else {
          ++n_bad;
          bool fresh = true;
          for (uint32_t k = 0; k < seen_bad_n; ++k) {
            if (seen_bad[k] == st->guest_base) { fresh = false; break; }
          }
          if (fresh && seen_bad_n < 512u) {
            seen_bad[seen_bad_n++] = st->guest_base;
            if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
              std::fprintf(f,
                           "MESHBAD vs=%016llX base=0x%08X stride=%u size=%u nv=%u "
                           "unit_normal=%u%% | region=0x%08X+%u size=%u resolved=%d\n",
                           (unsigned long long)vs_id, st->guest_base, st->stride, st->guest_size,
                           nv, pct, st->resource_base, st->view_offset, st->resource_size,
                           st->resolved ? 1 : 0);
              // The first vertices as the GUEST holds them, big-endian, so a
              // bad verdict can be read by eye against a good one.
              for (uint32_t v = 0; v < 3u && v < nv; ++v) {
                char line[256];
                int c = std::snprintf(line, sizeof(line), "MESHBAD   guest v%u", v);
                for (uint32_t o = 0; o + 4 <= st->stride && c < 220; o += 4) {
                  uint32_t d;
                  std::memcpy(&d, p + v * st->stride + o, 4);
                  c += std::snprintf(line + c, sizeof(line) - size_t(c), " %08X",
                                     __builtin_bswap32(d));
                }
                std::fprintf(f, "%s\n", line);
              }
              std::fflush(f);
              std::fclose(f);
            }
          }
        }
        // The other half, and the one that does not need luck: is the GPU's
        // copy of this range still the guest's bytes? A missed invalidation
        // shows up here the frame it happens, whether or not it has yet
        // deformed anything visible.
        {
          // PERSISTENCE, not a single sample. A one-off mismatch proves nothing:
          // the guest can be writing the range at this instant and the watch's
          // invalidation is drained at the top of the next Resolve, which would
          // fix it a frame later. A mismatch that survives many observations of
          // the SAME uploaded hash cannot be that -- the re-upload never came.
          struct StaleEntry {
            uint32_t base = 0;
            uint64_t uploaded = 0;
            uint32_t hits = 0;
            bool reported = false;
          };
          static StaleEntry stale[256];
          static uint32_t stale_n = 0;
          constexpr uint32_t kPersist = 200;
          const BufferSwap swap = st->endian == 2   ? BufferSwap::k8in32
                                  : st->endian == 1 ? BufferSwap::k8in16
                                                    : BufferSwap::kNone;
          uint32_t rbase = 0, rsize = 0;
          uint64_t up = 0, live = 0;
          if (buffers.VerifyRegion(st->guest_base, st->guest_size, swap, &rbase, &rsize, &up,
                                   &live) &&
              up != live) {
            ++n_stale;
            StaleEntry* e = nullptr;
            for (uint32_t k = 0; k < stale_n; ++k) {
              if (stale[k].base == rbase) { e = &stale[k]; break; }
            }
            if (!e && stale_n < 256u) {
              e = &stale[stale_n++];
              e->base = rbase;
            }
            if (e) {
              // A re-upload changes the stored hash; that is the region being
              // fixed, so the count starts over rather than accumulating across
              // two different staleness episodes.
              if (e->uploaded != up) {
                e->uploaded = up;
                e->hits = 0;
                e->reported = false;
              }
              if (++e->hits >= kPersist && !e->reported) {
                e->reported = true;
                if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
                  std::fprintf(f,
                               "MESHSTALE vs=%016llX base=0x%08X size=%u | region=0x%08X size=%u "
                               "swap=%u uploaded=%016llX live=%016llX unit_normal=%u%% "
                               "persistiu=%u\n",
                               (unsigned long long)vs_id, st->guest_base, st->guest_size, rbase,
                               rsize, uint32_t(swap), (unsigned long long)up,
                               (unsigned long long)live, pct, e->hits);
                  std::fflush(f);
                  std::fclose(f);
                }
              }
            }
          }
        }
        // Heartbeat, so "no MESHBAD lines" can be told apart from "the probe
        // never ran".
        static uint64_t tick = 0;
        if ((++tick % 20000u) == 0u) {
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "MESHCHK skinned draws: ok=%llu bad=%llu unreadable=%llu distintos_ruins=%u\n",
                         (unsigned long long)n_ok, (unsigned long long)n_bad,
                         (unsigned long long)n_unreadable, seen_bad_n);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
    }
  }
  // TEMP DIAG (PASSBIAS): the colour exponent bias of every 256x256 HDR pass.
  //
  // The water's reflection target (0x05AC3000, R16G16B16A16_FLOAT) comes out
  // ~33x brighter at the near camera than at the far one (mean RGB 0.99 vs
  // 0.03, alpha 11.9 vs 1.45). 33 is about 2^5, which is what an unapplied
  // RB_COLOR_INFO exponent bias looks like -- the Xenos pre-divides by 2^bias
  // on write and the resolve undoes it. Same class as
  // project-mcla-color-exp-bias.
  if (cfg.width == 256u && cfg.height == 256u && cfg.rt_format != 28u) {
    static std::set<uint64_t> seen_bias;
    const int32_t bias = ReadColorExpBias(base, dev);
    const uint64_t sig = (uint64_t(uint32_t(bias)) << 32) ^ cfg.rt_format ^ ps_id;
    if (seen_bias.size() < 24u && seen_bias.insert(sig).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        float inv[4];
        std::memcpy(inv, ps_bank.data() + 27u * 16u, 16);
        std::fprintf(f,
                     "PASSBIAS 256x256 rt_format=%u color_exp_bias=%d color_info=0x%08X "
                     "gInvColorExpBias=%.5f ps=%016llX\n",
                     cfg.rt_format, bias, rs.color_info, inv[0], (unsigned long long)ps_id);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // TEMP DIAG (WATER): every ocean/pond water draw, with the textures it binds.
  //
  // The sea is correctly dark from far away and blows out to white up close.
  // `xCityOceanWater__PSCityOceanWater` and its LOD twin are byte-identical in
  // their output path, so the difference is not the code -- it is what they are
  // fed. Both sample ReflectionSampler and WaveFoamSampler; a reflection target
  // that comes back white is the shape of the symptom.
  {
    const bool is_water = ps_id == 0x35F41762995C91B9ull ||   // seed do reflexo
                          ps_id == 0xC5C95CAE57E0D1C4ull ||   // xCityOceanWater
                          ps_id == 0x3E5818BE5A70E06Bull ||   // xCityOceanWaterLOD
                          ps_id == 0x18821F3A51B6E4DEull ||   // xCityOceanShore
                          ps_id == 0x2EB9178258B7EBDAull;     // xCityPondWater
    if (is_water) {
      static std::set<uint64_t> seen_water;
      uint64_t sig = ps_id ^ (uint64_t(bound_tex_count) << 56);
      for (uint32_t i = 0; i < bound_tex_count; ++i) {
        sig ^= (uint64_t(bound_tex[i].fetch.base_address) << 3) ^ bound_tex[i].fetch_slot;
      }
      // O passe que semeia o alvo de reflexo. Pular ele derruba o reflexo de
      // p50 0.8562 para 0.0064, entao e ele que enche o buffer de claro. Ele e
      // glow de luz: amostra LightGlowTexSampler e multiplica por StreakParams.y
      // e pela cor do vertice -- os tres candidatos estao nesta linha.
      if (ps_id == 0x35F41762995C91B9ull) {
        static bool seed_done = false;
        if (!seed_done) {
          seed_done = true;
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            float sp[4];
            std::memcpy(sp, ps_bank.data() + 64u * 16u, 16);
            std::fprintf(f, "SEEDRT StreakParams=%.5f %.5f %.5f %.5f into=%ux%u ntex=%u elems=%u",
                         sp[0], sp[1], sp[2], sp[3], cfg.width, cfg.height, bound_tex_count,
                         element_count);
            for (uint32_t i = 0; i < bound_tex_count && i < 6u; ++i) {
              const BoundTexture& b = bound_tex[i];
              std::fprintf(f, " | s%u=0x%08X %ux%u f%u src=%c", b.fetch_slot,
                           b.fetch.base_address, b.fetch.width, b.fetch.height, b.fetch.format,
                           TextureSourceTag(b.source));
            }
            std::fprintf(f, "\n");
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      // As constantes que o shader realmente recebe, DEPOIS de ApplyColorExpBias.
      // O mesmo shader desenha escuro na camera 50 e estourado na 51, com o
      // mesmo codigo e a mesma textura de reflexo, entao a diferenca esta aqui
      // ou no conteudo do reflexo. c_N fica no byte N*16 do banco.
      if (ps_id == 0xC5C95CAE57E0D1C4ull) {
        static bool wc_done = false;
        if (!wc_done) {
          wc_done = true;
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            const auto cst = [&](uint32_t n, const char* name) {
              float v[4];
              std::memcpy(v, ps_bank.data() + size_t(n) * 16u, 16);
              std::fprintf(f, "WATERCONST c%-3u %-18s %12.5f %12.5f %12.5f %12.5f\n", n,
                           name, v[0], v[1], v[2], v[3]);
            };
            cst(12, "gViewInverse0"); cst(13, "gViewInverse1");
            cst(14, "gViewInverse2"); cst(15, "gViewInverse3");
            cst(16, "gLightPosDir");  cst(26, "gLightAmbient");
            cst(27, "gInvColorExpBias"); cst(39, "gLightColor");
            cst(63, "ShaderGlobals");
            std::fprintf(f, "WATERCONST scene_bias=%d color_info=0x%08X into=%ux%u\n",
                         ReadColorExpBias(base, dev), rs.color_info, cfg.width, cfg.height);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      if (seen_water.size() < 48u && seen_water.insert(sig).second) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "WATER ps=%016llX vs=%016llX into=%ux%u ntex=%u elems=%u",
                       (unsigned long long)ps_id, (unsigned long long)vs_id, cfg.width,
                       cfg.height, bound_tex_count, element_count);
          for (uint32_t i = 0; i < bound_tex_count && i < 8u; ++i) {
            const BoundTexture& b = bound_tex[i];
            std::fprintf(f, " | s%u=0x%08X %ux%u f%u src=%c", b.fetch_slot, b.fetch.base_address,
                         b.fetch.width, b.fetch.height, b.fetch.format,
                         TextureSourceTag(b.source));
          }
          std::fprintf(f, "\n");
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
  }
  // TEMP DIAG (PANEL): every draw that SAMPLES the 960x640 Flash/vhsm UI surface,
  // whatever it renders into. RenderDoc cannot answer this -- its
  // GetReadOnlyResources does not enumerate a bindless bind, so a capture shows
  // these draws with no texture at all. The runtime knows the fetch, so it can.
  for (uint32_t i = 0; i < bound_tex_count; ++i) {
    const BoundTexture& b = bound_tex[i];
    if (b.fetch.width != 960u || b.fetch.height != 640u) continue;
    static uint32_t seen_panel[64];
    static uint32_t seen_panel_n = 0;
    // The vertex COLOR alpha over time, throttled. The panel composites at
    // alpha 7/255, which is either a fade-in that never advances or a value the
    // game really means. One sample cannot tell the two apart; a series can.
    if (inline_geom && inline_geom->address && inline_geom->stride >= 28u) {
      static uint32_t tick = 0;
      if ((tick++ % 120u) == 0u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "PANELFADE ps=%016llX color=%08X uv0=%08X\n",
                       (unsigned long long)ps_id, R32(base, inline_geom->address + 24),
                       R32(base, inline_geom->address + 28));
          std::fflush(f); std::fclose(f);
        }
      }
    }
    // Every instance, not one per shader: the composite and the sprites inside
    // the panel share xrage_im__PS_Textured, so deduplicating by ps_id showed
    // one arbitrary quad and hid the one that matters. Capped by count instead.
    if (seen_panel_n >= 40u) continue;
    ++seen_panel_n;
    if (inline_geom && inline_geom->address && inline_geom->stride >= 32u) {
      // Offsets from THIS draw's own declaration. Hardcoding 0/4/24 read
      // whatever happened to sit there: the colours came out with a top byte
      // wandering between 0x00 and 0x17 and the rectangles landed in a corner
      // the menu never occupies, which is the signature of reading a shifted
      // field, not of a game drawing at 3% alpha.
      const uint32_t st = inline_geom->stride;
      uint32_t pos_off = 0xFFFFFFFFu, col_off = 0xFFFFFFFFu, uv_off = 0xFFFFFFFFu;
      for (const auto& el : bound.input_layout) {
        if (!el.semantic_name) continue;
        if (el.semantic_index == 0 && std::strcmp(el.semantic_name, "POSITION") == 0)
          pos_off = el.aligned_byte_offset;
        else if (el.semantic_index == 0 && std::strcmp(el.semantic_name, "COLOR") == 0)
          col_off = el.aligned_byte_offset;
        else if (el.semantic_index == 0 && std::strcmp(el.semantic_name, "TEXCOORD") == 0)
          uv_off = el.aligned_byte_offset;
      }
      const uint32_t nv = std::min<uint32_t>(inline_geom->size_bytes / st, 6u);
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      uint32_t col = 0;
      if (pos_off != 0xFFFFFFFFu) {
        for (uint32_t v = 0; v < nv; ++v) {
          const uint32_t at = inline_geom->address + v * st;
          uint32_t rx = R32(base, at + pos_off), ry = R32(base, at + pos_off + 4);
          float fx, fy;
          std::memcpy(&fx, &rx, 4);
          std::memcpy(&fy, &ry, 4);
          if (fx < x0) x0 = fx;
          if (fx > x1) x1 = fx;
          if (fy < y0) y0 = fy;
          if (fy > y1) y1 = fy;
          if (col_off != 0xFFFFFFFFu) col = R32(base, at + col_off);
        }
      }
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "PANELQUAD ps=%016llX rect=(%.0f,%.0f..%.0f,%.0f) color=%08X into=%ux%u "
                     "vp=%.0fx%.0f stride=%u off(p/c/uv)=%u/%u/%u\n",
                     (unsigned long long)ps_id, x0, y0, x1, y1, col, cfg.width, cfg.height,
                     hv.width, hv.height, st, pos_off, col_off, uv_off);
        std::fflush(f); std::fclose(f);
      }
    }
    const uint32_t sig = uint32_t(ps_id) ^ (b.fetch.base_address << 1);
    bool fresh = true;
    for (uint32_t k = 0; k < 0u; ++k) if (seen_panel[k] == sig) { fresh = false; break; }
    if (!fresh) continue;
    if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
      std::fprintf(f,
                   "PANELREAD ps=%016llX slot=%u addr=0x%08X %ux%u src=%c into=%ux%u "
                   "alphatest=%d func=%u ref=0x%08X blend0=0x%08X mask=0x%X\n",
                   (unsigned long long)ps_id, b.fetch_slot, b.fetch.base_address, b.fetch.width,
                   b.fetch.height, TextureSourceTag(b.source), cfg.width, cfg.height,
                   rs.alpha_test_enable ? 1 : 0, rs.alpha_func, R32(base, dev + kDevRegAlphaRef),
                   rs.blend_control0, rs.color_mask);
      // The vertex layout too: this shader multiplies the texture by the vertex
      // COLOR (oC0 = tex * iColor0), so a COLOR attribute read at the wrong
      // offset or format makes the whole panel come out with alpha zero, which
      // is exactly what the pixel history shows.
      for (const auto& el : bound.input_layout) {
        std::fprintf(f, "PANELVTX   attr %s[%u] fmt=%u slot=%u off=%u\n",
                     el.semantic_name ? el.semantic_name : "?", el.semantic_index,
                     el.dxgi_format, el.input_slot, el.aligned_byte_offset);
      }
      for (size_t si = 0; si < bound.streams.size(); ++si) {
        std::fprintf(f, "PANELVTX   stream%zu stride=%u guest_size=%u endian=%u inline=%d\n",
                     si, bound.streams[si].stride, bound.streams[si].guest_size,
                     bound.streams[si].endian, inline_geom ? 1 : 0);
      }
      // The vertices themselves. oC0 = tfetch2D(...) * iColor0, the texture's
      // alpha is 255 across the menu box, and the draw comes out with alpha 0 --
      // so either the vertex COLOR carries alpha 0 or the UV lands on a
      // transparent texel. Both answers live in this buffer.
      // NOT inline-only. The draw that actually composites the panel --
      // xGloss__PS_TexturedGloss, a lit 3D card -- comes through an ordinary
      // vertex buffer, so every earlier dump here printed nothing for it and
      // the one draw that matters was the one never measured. Its bound stream
      // is read the same way, behind the readability check guest addresses out
      // of a fetch constant always need.
      const uint32_t vin_base =
          inline_geom ? inline_geom->address
                      : (bound.streams.empty() ? 0u : bound.streams[0].guest_base);
      const uint32_t vin_stride =
          inline_geom ? inline_geom->stride
                      : (bound.streams.empty() ? 0u : bound.streams[0].stride);
      const uint32_t vin_size =
          inline_geom ? inline_geom->size_bytes
                      : (bound.streams.empty() ? 0u : bound.streams[0].guest_size);
      std::fprintf(f, "PANELVTX   vs=%016llX streams=%zu vin=0x%08X stride=%u size=%u\n",
                   (unsigned long long)vs_id, bound.streams.size(), vin_base, vin_stride,
                   vin_size);
      // An inline draw's vertices sit in the command buffer at a VIRTUAL
      // address; a bound stream's base comes out of the fetch constant and is
      // PHYSICAL. Reading the second one through the virtual membase lands on
      // unmapped pages -- which is why the first attempt printed the header
      // line and no vertices at all for the one draw being chased.
      const uint8_t* phys = inline_geom ? nullptr : TranslatePhysicalGuest(vin_base);
      const bool vin_ok =
          vin_base && vin_stride &&
          (inline_geom
               ? IsGuestRangeReadable(vin_base, std::min<uint32_t>(vin_size, 6u * vin_stride))
               : (phys != nullptr &&
                  IsPhysicalRangeReadable(vin_base, std::min<uint32_t>(vin_size, 6u * vin_stride))));
      if (vin_ok) {
        const uint32_t stride = vin_stride;
        const uint32_t nv = std::min<uint32_t>(vin_size / stride, 6u);
        for (uint32_t v = 0; v < nv; ++v) {
          const uint32_t at = vin_base + v * stride;
          char line[256];
          int n = std::snprintf(line, sizeof(line), "PANELVTX   v%u", v);
          for (uint32_t off = 0; off + 4 <= stride && n < 220; off += 4) {
            uint32_t raw;
            if (phys) {
              std::memcpy(&raw, phys + (v * stride) + off, 4);
              raw = __builtin_bswap32(raw);
            } else {
              raw = R32(base, at + off);
            }
            float fv;
            std::memcpy(&fv, &raw, 4);
            n += std::snprintf(line + n, sizeof(line) - size_t(n), " [%u]=%08X/%.3f", off, raw, fv);
          }
          std::fprintf(f, "%s\n", line);
        }
      }
      std::fflush(f); std::fclose(f);
    }
  }
  // TEMP DIAG (UIQUAD): every inline 2D quad that lands on a display-sized
  // colour target, with its screen rectangle, its vertex colour and the texture
  // it samples. Scoping the earlier dump to draws that sample the 960x640
  // surface showed only the HUD speedometer -- the menu never appeared in it,
  // which is the question this answers directly.
  // NOT inline-only. Every quad dump before this one required inline_geom, so a
  // menu composite issued as an ordinary DrawVertices/DrawIndexedVertices was
  // invisible to all of them -- which is how "the geometry is never emitted"
  // got concluded from a filter that could not have seen it.
  const uint32_t vtx_base =
      inline_geom ? inline_geom->address
                  : (bound.streams.empty() ? 0u : bound.streams[0].guest_base);
  const uint32_t vtx_stride =
      inline_geom ? inline_geom->stride
                  : (bound.streams.empty() ? 0u : bound.streams[0].stride);
  const uint32_t vtx_size =
      inline_geom ? inline_geom->size_bytes
                  : (bound.streams.empty() ? 0u : bound.streams[0].guest_size);
  // The guest base of a non-inline stream comes straight out of the fetch
  // constant and is NOT guaranteed readable by the CPU -- reading it blind
  // segfaulted the process on the first try. Everything else in this file that
  // touches guest memory by address goes through this check first.
  // The `cfg.width >= 1024` this used to carry was the blind spot: RenderDoc
  // reports the menu's composite draw with viewport/scissor 960x640 and the
  // 1280x720 surface as its RTV, and the runtime derives the pass shape from
  // the VIEWPORT -- so that draw was excluded from the very census that
  // concluded "the geometry is never emitted". Any colour pass now, with the
  // pass shape and the guest's surface pitch printed alongside.
  if (vtx_base && vtx_stride >= 16u && vtx_size >= vtx_stride &&
      IsGuestRangeReadable(vtx_base, std::min<uint32_t>(vtx_size, 6u * vtx_stride)) &&
      cfg.rt_format == 28u) {
    // Deduplicated by rectangle and texture, not capped by count: the HUD
    // repeats the same handful of quads every frame and a plain counter was
    // spent long before the pause menu could be opened.
    static uint32_t uq_seen[512];
    static uint32_t uq_n = 0;
    if (uq_n < 512u) {
      uint32_t pos_off = 0xFFFFFFFFu, col_off = 0xFFFFFFFFu;
      for (const auto& el : bound.input_layout) {
        if (!el.semantic_name || el.semantic_index != 0) continue;
        if (std::strcmp(el.semantic_name, "POSITION") == 0) pos_off = el.aligned_byte_offset;
        else if (std::strcmp(el.semantic_name, "COLOR") == 0) col_off = el.aligned_byte_offset;
      }
      if (pos_off != 0xFFFFFFFFu) {
        const uint32_t st = vtx_stride;
        const uint32_t nv = std::min<uint32_t>(vtx_size / st, 6u);
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        uint32_t col = 0;
        for (uint32_t v = 0; v < nv; ++v) {
          const uint32_t at = vtx_base + v * st;
          uint32_t rx = R32(base, at + pos_off), ry = R32(base, at + pos_off + 4);
          float fx, fy;
          std::memcpy(&fx, &rx, 4);
          std::memcpy(&fy, &ry, 4);
          if (fx < x0) x0 = fx;
          if (fx > x1) x1 = fx;
          if (fy < y0) y0 = fy;
          if (fy > y1) y1 = fy;
          if (col_off != 0xFFFFFFFFu) col = R32(base, at + col_off);
        }
        // Only quads big enough to be a panel AND inside the rectangle the pause
        // menu occupies (measured off the emulated path, which draws it
        // correctly: x 377..580, y 313..460 in the guest's 1280x720 UI space).
        // Without the region filter the HUD and the minimap exhaust the budget
        // before the menu is ever opened.
        // The full-screen UI composite: one quad covering the whole guest UI
        // space. It is the draw that puts the Flash surface on the frame, and
        // the one whose sampled texture was never identified -- RenderDoc shows
        // it with no texture at all because the bind is bindless.
        if (x0 <= 2.0f && y0 <= 2.0f && x1 >= 1278.0f && y1 >= 718.0f) {
          static uint32_t fs = 0;
          if (fs++ < 8u) {
            if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
              std::fprintf(f, "FULLSCREENUI color=%08X ntex=%u", col, bound_tex_count);
              for (uint32_t t = 0; t < bound_tex_count && t < 4u; ++t) {
                std::fprintf(f, " | slot%u addr=0x%08X %ux%u fmt=%u src=%c",
                             bound_tex[t].fetch_slot, bound_tex[t].fetch.base_address,
                             bound_tex[t].fetch.width, bound_tex[t].fetch.height,
                             bound_tex[t].fetch.format, TextureSourceTag(bound_tex[t].source));
              }
              std::fprintf(f, " ps=%016llX\n", (unsigned long long)ps_id);
              std::fflush(f); std::fclose(f);
            }
          }
        }
        // The region box that used to sit here was in the guest's 1280x720 UI
        // space, so it could not match a quad issued through a smaller viewport
        // either. Size alone now; the rect+texture dedup keeps the repeating
        // HUD from spending the table.
        if ((x1 - x0) >= 40.0f && (y1 - y0) >= 25.0f) {
          const uint32_t rsig = (uint32_t(x0) & 0x7FFu) | ((uint32_t(y0) & 0x7FFu) << 11) |
                                ((uint32_t(x1 - x0) & 0x3FFu) << 22);
          const uint32_t tsig = bound_tex_count ? bound_tex[0].fetch.base_address : 0u;
          const uint32_t sig = rsig ^ (tsig << 3) ^ (tsig >> 13);
          bool fresh = true;
          for (uint32_t k = 0; k < uq_n; ++k) if (uq_seen[k] == sig) { fresh = false; break; }
          if (!fresh) return_if_seen: { }
          if (!fresh) goto uiquad_done;
          uq_seen[uq_n++] = sig;
          uint32_t t_addr = 0, t_w = 0, t_h = 0;
          char t_src = '-';
          if (bound_tex_count) {
            t_addr = bound_tex[0].fetch.base_address;
            t_w = bound_tex[0].fetch.width;
            t_h = bound_tex[0].fetch.height;
            t_src = TextureSourceTag(bound_tex[0].source);
          }
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            // The surface the quad was AUTHORED for, read off gWorldViewProj
            // (c8..c11 of the VS bank). An orthographic UI matrix carries
            // 2/width in [0] and -2/height in [5], so this says what the guest
            // thinks it is drawing into -- independently of the viewport
            // registers the pass shape is derived from. Where the two disagree,
            // the runtime is routing the draw to the wrong target.
            const float* wvp = reinterpret_cast<const float*>(vs_bank.data() + 8 * 16);
            const float ow = (std::isfinite(wvp[0]) && wvp[0] != 0.0f) ? 2.0f / wvp[0] : 0.0f;
            const float oh = (std::isfinite(wvp[5]) && wvp[5] != 0.0f) ? -2.0f / wvp[5] : 0.0f;
            std::fprintf(f,
                         "UIQUAD rect=(%.0f,%.0f..%.0f,%.0f) pass=%ux%u pitch=%u gm=%u "
                         "ortho=%.0fx%.0f color=%08X tex=0x%08X %ux%u src=%c "
                         "inline=%d stride=%u ps=%016llX\n",
                         x0, y0, x1, y1, cfg.width, cfg.height, rs.surface_info & 0x3FFFu,
                         rs.msaa_samples, ow, oh, col, t_addr, t_w, t_h, t_src,
                         inline_geom ? 1 : 0, vtx_stride, (unsigned long long)ps_id);
            std::fflush(f); std::fclose(f);
          }
        }
        uiquad_done:;
      }
    }
  }
  // TEMP DIAG (DISPDRAW): a plain inventory of everything that renders into the
  // display-sized colour target -- shader, first texture, vertex count. No
  // rectangle filter and no assumption about where POSITION sits in the vertex,
  // because every earlier dump that DID assume one reported "no menu geometry"
  // and that conclusion turned out to rest on the filter, not on the frame.
  if (cfg.rt_format == 28u && cfg.width >= 1024u) {
    static uint32_t seen_dd[512];
    static uint32_t seen_dd_n = 0;
    const uint32_t taddr = bound_tex_count ? bound_tex[0].fetch.base_address : 0u;
    const uint32_t sig = uint32_t(ps_id) ^ (taddr << 1) ^ (taddr >> 17);
    bool fresh = true;
    for (uint32_t k = 0; k < seen_dd_n; ++k) {
      if (seen_dd[k] == sig) { fresh = false; break; }
    }
    if (fresh && seen_dd_n < 512u) {
      seen_dd[seen_dd_n++] = sig;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "DISPDRAW ps=%016llX tex=0x%08X %ux%u src=%c ntex=%u verts=%u prim=%u "
                     "inline=%d\n",
                     (unsigned long long)ps_id, taddr,
                     bound_tex_count ? bound_tex[0].fetch.width : 0u,
                     bound_tex_count ? bound_tex[0].fetch.height : 0u,
                     bound_tex_count ? TextureSourceTag(bound_tex[0].source) : '-',
                     bound_tex_count, element_count, primitive_type,
                     inline_geom ? 1 : 0);
        std::fflush(f); std::fclose(f);
      }
    }
  }
  // TEMP DIAG (SRVMAP): descriptor index -> what the runtime actually put there,
  // for every draw that renders into a UI-sized colour target. RenderDoc can
  // read the index out of the shader's constants but cannot follow a bindless
  // heap slot to its resource; the runtime can. This is how a capture's
  // "TextureSampler_Texture2DDescriptorIndex = N" becomes a guest address.
  if (cfg.rt_format == 28u && cfg.width >= 640u) {
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      static uint32_t seen_srv[256];
      static uint32_t seen_srv_n = 0;
      bool fresh = true;
      for (uint32_t k = 0; k < seen_srv_n; ++k) {
        if (seen_srv[k] == b.srv_descriptor_index) { fresh = false; break; }
      }
      if (!fresh || seen_srv_n >= 256u) continue;
      seen_srv[seen_srv_n++] = b.srv_descriptor_index;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "SRVMAP idx=%u slot=%u addr=0x%08X %ux%u fmt=%u src=%c into=%ux%u "
                     "ps=%016llX\n",
                     b.srv_descriptor_index, b.fetch_slot, b.fetch.base_address, b.fetch.width,
                     b.fetch.height, b.fetch.format, TextureSourceTag(b.source), cfg.width,
                     cfg.height, (unsigned long long)ps_id);
        std::fflush(f); std::fclose(f);
      }
    }
  }
  // TEMP DIAG (UITEX): what the UI pass asks for and what the bridge handed it.
  // The pause menu draws its panel with a textured shader; a census of the
  // capture found twelve such draws on the display target with NO texture bound
  // at all, which is a different failure from binding the wrong one.
  if (is_display) {
    static uint32_t seen[192];
    static uint32_t seen_n = 0;
    if (bound_tex_count == 0) {
      const uint32_t sig = uint32_t(ps_id) ^ 0x8000000u;
      bool fresh = true;
      for (uint32_t i = 0; i < seen_n; ++i) if (seen[i] == sig) { fresh = false; break; }
      if (fresh && seen_n < 192u) {
        seen[seen_n++] = sig;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "UITEX ps=%016llX NENHUMA TEXTURA LIGADA elems=%zu\n",
                       (unsigned long long)ps_id, geom.input_layout.size());
          std::fflush(f); std::fclose(f);
        }
      }
    }
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      // Only the failures. Logging every bound texture spent the whole budget on
      // ordinary world art before the menu was even opened -- 192 entries, all
      // of them a healthy guest decode or render-target bridge.
      if (b.source == TextureSource::kGuestDecode ||
          b.source == TextureSource::kRenderTargetBridge) {
        continue;
      }
      const uint32_t sig = b.fetch.base_address ^ (b.fetch.width << 3) ^ (b.fetch.height << 17);
      bool fresh = true;
      for (uint32_t k = 0; k < seen_n; ++k) if (seen[k] == sig) { fresh = false; break; }
      if (!fresh || seen_n >= 192u) continue;
      seen[seen_n++] = sig;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f,
                     "UITEX ps=%016llX slot=%u addr=0x%08X %ux%u fmt=%u src=%c resolved=%d\n",
                     (unsigned long long)ps_id, b.fetch_slot, b.fetch.base_address, b.fetch.width,
                     b.fetch.height, b.fetch.format, TextureSourceTag(b.source),
                     b.resolved ? 1 : 0);
        std::fflush(f); std::fclose(f);
      }
    }
  }
  g_binder_stats_for_report = binder.stats();
  // TEMP DIAG (remove after): two things never tested about the minimap.
  //
  // MMSRC: the 220x220 minimap is render-to-texture -- drawn into, then SAMPLED
  // by the composite. If that sample comes from a guest-memory decode instead of
  // the live render target, the circular punch (which writes alpha into the
  // render target) never reaches the screen, because the native runtime does not
  // write resolved pixels back to guest memory. The punch would look perfect and
  // be invisible at the same time -- exactly the state this investigation is in.
  //
  // MMSAMP: the sampler the mask is read through. Only WHICH texture sat in
  // fetch slot 0 was ever logged, never how it is filtered or clamped.
  {
    static std::set<uint64_t> seen_src;
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      const bool is_minimap = b.fetch.width == 220 && b.fetch.height == 220;
      const bool is_mask = cfg.width == 220 && cfg.height == 220 && b.fetch_slot == 0;
      if (!is_minimap && !is_mask) continue;
      const uint64_t sig = (uint64_t(b.fetch.base_address) << 12) ^
                           (uint64_t(uint32_t(b.source)) << 4) ^ uint64_t(b.fetch_slot) ^
                           (is_mask ? 0x80000000ull : 0ull);
      if (!seen_src.insert(sig).second) continue;
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        static const char* kSrc[] = {"UNRESOLVED", "RT_BRIDGE", "GUEST_DECODE", "FALLBACK"};
        std::fprintf(f, "%s slot=%u 0x%08X %ux%u f%u src=%s | clamp=%u,%u mag=%u min=%u mip=%u aniso=%u lodbias=%d border=%u | into=%ux%u\n",
                     is_mask ? "MMSAMP" : "MMSRC", b.fetch_slot, b.fetch.base_address,
                     b.fetch.width, b.fetch.height, b.fetch.format,
                     kSrc[uint32_t(b.source) & 3u], b.sampler.clamp_x, b.sampler.clamp_y,
                     b.sampler.mag_filter, b.sampler.min_filter, b.sampler.mip_filter,
                     b.sampler.aniso_filter, b.sampler.lod_bias_raw, b.sampler.border_color,
                     cfg.width, cfg.height);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  g_buffer_stats_for_report = buffers.stats();
  g_texture_stats_for_report = textures.stats();
  uint32_t resolved_tex = 0;
  for (uint32_t i = 0; i < bound_tex_count; ++i) {
    NoteTexture(bound_tex[i].fetch.format, bound_tex[i].resolved);
    if (bound_tex[i].resolved) {
      ++resolved_tex;
    } else {
      NoteUnresolved(bound_tex[i]);
    }
  }

  SharedConstantValues shared_values;
  // Which TEXCOORD semantics arrive with their components swapped, because a
  // 16-bit pair rode in a stream the buffer cache had to swap at 32-bit width.
  // BuildGeometrySnapshot derives the mask from the declaration; the translated
  // vertex shader undoes it with value.yxwz. See geometry.cpp for the capture
  // this was measured in.
  if (REXCVAR_GET(mcla_native_gfx_swapped_texcoords)) {
    shared_values.swapped_texcoords = bound.swapped_texcoords;
  }
  // The register-driven threshold is the default. It reads RB_ALPHA_REF and
  // steps to the next representable float above it, because the guest compares
  // with > and the shader clips with >=. That step has to skip the denormals:
  // the game does use a ref of zero, and next-above-zero is 1.4e-45, which the
  // shader's flush-to-zero turns back into 0 and nothing gets clipped -- that
  // was the green squares on tree foliage. mcla_native_gfx_alpha_ref off falls
  // back to the bring-up's fixed 0.5, which made alpha-tested cars invisible.
  shared_values.alpha_threshold = REXCVAR_GET(mcla_native_gfx_alpha_ref)
                                      ? AlphaTestThreshold(rs)
                                      : (rs.alpha_test_enable ? 0.5f : 0.0f);
  // Every translated vertex shader ends with
  //   SV_Position.xy = clip.xy + g_HalfPixelOffset.xy * clip.w
  // which is the D3D9 -> D3D10+ pixel-centre correction the guest hardware got
  // for free. Left at zero the whole image sits half a pixel off: the 2D pass
  // is where it shows, because a UI quad that should land exactly on a texel
  // grid instead samples between texels and every sprite edge and glyph comes
  // out soft or one pixel wide.
  //
  // screen_x = ndc_x * (W/2) + (X + W/2) and screen_y = ndc_y * (-H/2) + ...,
  // so half a pixel is 1/W in x and 1/H in y, measured against the VIEWPORT,
  // not the target. The signs below give -0.5 screen pixels on both axes.
  //
  // KNOWN DEFECT, and the obvious repair was measured and REJECTED. At -0.5 the
  // right and bottom edges of a full-target quad land exactly on the centre of
  // the last pixel, and D3D's top-left fill rule excludes a centre sitting on a
  // right or bottom edge, so the last row and column are never rasterised. In
  // the 1280x720 composite dump, row 719 came out as ONE distinct value, mean
  // 6.667 = kClearColor {0.02, 0.02, 0.04} as 8-bit [5, 5, 10]; on screen a
  // two-pixel dark band along the bottom and right, two because 1280x720 is
  // upscaled 1.5x to 1920x1080.
  //
  // Flipping to +0.5 -- which is what the emulated path adds, in
  // src/graphics/util/draw.cpp -- removes the band and makes it WORSE. Measured
  // by correlating the composite dump against the anchor dump of the same
  // frame, over offsets -2..+2:
  //
  //     half_pixel off   X peak +0 (0.8465)   Y peak +0 (0.8614)   no band
  //     half_pixel +0.5  X peak +1 (0.4864)   Y peak +1 (0.6548)   row 0 == row 1
  //
  // The whole image moves a full pixel and the first row and column become
  // bit-identical duplicates. The reason is that this offset is applied to
  // EVERY draw: the scene is shifted once when it is rendered into the anchor,
  // and the fullscreen composite quad is shifted again when it samples that
  // anchor, so the two compound into one pixel. No sign fixes that.
  //
  // The real repair is the condition the emulated path has and this does not:
  // it applies the offset only when PA_SU_VTX_CNTL.pix_center == kD3DZero, so a
  // pass whose quad is already in screen space is left alone. That needs the
  // register's offset in the guest device shadow, which has not been reverse
  // engineered yet. Until then this stays at -0.5: it keeps the alignment the
  // 2D pass was tuned against and costs one row and one column at the edge.
  // mcla_native_gfx_half_pixel=false removes the band and measures as the best
  // aligned of the three, but gives up the pixel-grid alignment the offset was
  // added for -- check UI sharpness before trusting it.
  // +0.5 SCREEN pixels on both axes, and ONLY for a pass that asks for the
  // Direct3D 9 convention. Xenos samples at integer positions and D3D12 at
  // k + 0.5, so the geometry has to move FORWARD half a pixel for the host
  // sample to land where the guest's did -- which is exactly what the emulated
  // path adds (src/graphics/util/draw.cpp: offset_add_xy += 0.5f, guarded by
  // pix_center == kD3DZero). NDC x is +1/W for half a pixel right; NDC y is
  // -1/H because NDC y points up while screen y points down.
  const bool pix_center_zero =
      (rs.pa_su_vtx_cntl & uint32_t(1)) == xenos_pix_center_d3d_zero;
  if (REXCVAR_GET(mcla_native_gfx_half_pixel) && pix_center_zero && hv.width > 0.0f &&
      hv.height > 0.0f) {
  {  // TEMP DIAG (A2M): quem pede alpha-to-mask, e com que referencia de alpha.
    static std::set<uint64_t> seen_a2m;
    const uint64_t combo = (uint64_t(cfg.width) << 40) | (uint64_t(cfg.height) << 16) |
                           (rs.color_control & 0x1Fu);
    if (seen_a2m.size() < 40u && seen_a2m.insert(combo).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "A2M target=%ux%u colorcontrol=0x%08X a2m=%d alphatest=%d func=%u\n",
                     cfg.width, cfg.height, rs.color_control,
                     rs.alpha_to_mask_enable ? 1 : 0, rs.alpha_test_enable ? 1 : 0,
                     rs.alpha_func);
        std::fclose(f);
      }
    }
  }
    {  // TEMP DIAG (PIXCENTER): distribution of PA_SU_VTX_CNTL per pass.
      static std::set<uint64_t> seen;
      const uint64_t combo = (uint64_t(cfg.width) << 40) | (uint64_t(cfg.height) << 16) |
                             (rs.pa_su_vtx_cntl & 0xFFFFu);
      if (seen.size() < 40u && seen.insert(combo).second) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "PIXCENTER raw=0x%08X pix_center=%u target=%ux%u vp=%.1fx%.1f\n",
                       rs.pa_su_vtx_cntl, rs.pa_su_vtx_cntl & 1u, cfg.width, cfg.height,
                       hv.width, hv.height);
          std::fclose(f);
        }
      }
    }
    shared_values.half_pixel_offset[0] = 1.0f / hv.width;
    shared_values.half_pixel_offset[1] = -1.0f / hv.height;
  }
  std::memcpy(shared.data() + kSharedBooleansByteOffset, &shared_values.booleans, 4);
  std::memcpy(shared.data() + kSharedSwappedTexcoordsByteOffset, &shared_values.swapped_texcoords,
              4);
  std::memcpy(shared.data() + kSharedHalfPixelOffsetByteOffset, shared_values.half_pixel_offset, 8);
  // Xenos applies the source blend factor before a MIN/MAX blend op; D3D12
  // ignores the factors for those ops, so the pixel shader folds the factor into
  // its own output and these say which factor to fold. Left at zero -- the
  // shader's "leave it alone" mode -- for every draw that does not hit the case,
  // which is nearly all of them. Colour and alpha are independent equations, so
  // they get independent modes.
  {
    const uint32_t bc = rs.blend_control0;
    shared_values.blend_premult_rgb = uint32_t(BlendPremultFor(
        (bc >> 5) & 0x7u, bc & 0x1Fu, (bc >> 8) & 0x1Fu));
    shared_values.blend_premult_a = uint32_t(BlendPremultFor(
        (bc >> 21) & 0x7u, (bc >> 16) & 0x1Fu, (bc >> 24) & 0x1Fu));
    // The CONSTANT_COLOR / CONSTANT_ALPHA modes read this; it is the same value
    // OMSetBlendFactor gets, so the two halves cannot disagree.
    std::memcpy(shared_values.blend_premult_constant, rs.blend_constant, 16);
  }
  std::memcpy(shared.data() + kSharedAlphaThresholdByteOffset, &shared_values.alpha_threshold, 4);
  std::memcpy(shared.data() + kSharedBlendPremultRgbByteOffset, &shared_values.blend_premult_rgb, 4);
  std::memcpy(shared.data() + kSharedBlendPremultAByteOffset, &shared_values.blend_premult_a, 4);
  std::memcpy(shared.data() + kSharedBlendPremultConstByteOffset,
              shared_values.blend_premult_constant, 16);
  // TEMP DIAG (remove after): the punch that clips the minimap to a circle is
  // the draw into the 220x220 target whose blend op is REV_SUBTRACT. The UV and
  // the mask texture both check out now, so this reports what the draw actually
  // receives: which texture landed in fetch slot 0 (the shader MaskSampler),
  // whether it resolved, the premult mode folded into the shader output, and
  // the alpha threshold -- the shader clips on `oC0.w - g_AlphaThreshold`.
  if (cfg.width == 220 && cfg.height == 220) {
    // Order matters and was never checked: a punch that runs BEFORE the road
    // draws erases an empty buffer, and the roads are then painted over
    // everything -- indistinguishable from a punch that does nothing.
    static uint32_t mm_frame = 0xFFFFFFFFu;
    static uint32_t mm_index = 0;
    // TEMP DIAG (MINIMAPORDER): the punch's position within the pass, against
    // the pass's total. The suspicion above is now the only one left standing:
    // the mask, the blend, the shader, the geometry, the target resource and
    // the colour write mask all measured correct, and the draw does reach a
    // Draw* call, yet the 220x220 alpha has no circle in it. A punch that runs
    // before the roads erases an empty buffer and is then painted over.
    static uint32_t mm_punches[16];
    static uint32_t mm_punch_n = 0;
    if (mm_frame != context.frame_index()) {
      if (mm_frame != 0xFFFFFFFFu && mm_index) {
        static uint32_t reported = 0;
        if (reported++ < 400u) {
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "MINIMAPORDER frame=%u draws=%u punches em:", mm_frame, mm_index);
            for (uint32_t i = 0; i < mm_punch_n; ++i) std::fprintf(f, " #%u", mm_punches[i]);
            std::fprintf(f, "%s\n", mm_punch_n ? "" : " (nenhum)");
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      mm_frame = context.frame_index();
      mm_index = 0;
      mm_punch_n = 0;
    }
    ++mm_index;
    if (rs.blend_control0 == 0x01810181u && mm_punch_n < 16u) {
      mm_punches[mm_punch_n++] = mm_index;
    }
    static std::set<uint64_t> seen_mmst;
    const uint32_t blend_op_rgb = (rs.blend_control0 >> 5) & 0x7u;
    const uint32_t blend_op_a = (rs.blend_control0 >> 21) & 0x7u;
    uint64_t sig = (uint64_t(blend_op_rgb) << 40) ^ (uint64_t(blend_op_a) << 32) ^
                   (uint64_t(mm_index) << 8) ^ uint64_t(bound_tex_count);
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      sig ^= (uint64_t(bound_tex[i].fetch.base_address) << 3) ^ uint64_t(bound_tex[i].fetch_slot);
    }
    // TEMP DIAG (remove after): for the PUNCH specifically -- the draw into the
    // 220x220 target whose blend op is REV_SUBTRACT on both channels -- read the
    // GUEST memory behind whatever texture sits in fetch slot 0, the shader
    // MaskSampler. Reading guest memory rather than the decoded resource keeps
    // this independent of the texture cache: it answers "is the mask white in
    // the game's own memory" for whichever texture the punch really uses, be it
    // the 32x32 or the 256x256 DXT1 that also appears in this slot.
    if (blend_op_rgb == 4u && blend_op_a == 4u) {
      static std::set<uint32_t> seen_punch;
      for (uint32_t i = 0; i < bound_tex_count; ++i) {
        const BoundTexture& b = bound_tex[i];
        if (b.fetch_slot != 0 || !seen_punch.insert(b.fetch.base_address).second) continue;
        const uint32_t probe = 4096u;
        uint32_t ones = 0, zeros = 0, other = 0;
        uint32_t head[6] = {};
        // PHYSICAL, not virtual: the texture cache reads this address through
        // TranslatePhysicalGuest / IsPhysicalRangeReadable, and the virtual
        // predicate says "not readable" for it -- which silently made the first
        // version of this probe read nothing and report 0% for every bucket.
        const uint8_t* g = IsPhysicalRangeReadable(b.fetch.base_address, probe)
                               ? TranslatePhysicalGuest(b.fetch.base_address)
                               : nullptr;
        if (g) {
          for (uint32_t k = 0; k < probe; ++k) {
            if (g[k] == 0xFFu) ++ones; else if (g[k] == 0x00u) ++zeros; else ++other;
          }
          for (uint32_t k = 0; k < 6; ++k) std::memcpy(&head[k], g + k * 4, 4);
        }
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "PUNCHTEX 0x%08X %ux%u f%u tiled=%d endian=%u src=%u | guest 4KiB: 0xFF=%.1f%% 0x00=%.1f%% outros=%.1f%% | head %08X %08X %08X %08X %08X %08X\n",
                       b.fetch.base_address, b.fetch.width, b.fetch.height, b.fetch.format,
                       b.fetch.tiled ? 1 : 0, b.fetch.endianness, uint32_t(b.source),
                       100.0 * ones / probe, 100.0 * zeros / probe, 100.0 * other / probe,
                       head[0], head[1], head[2], head[3], head[4], head[5]);
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    if (seen_mmst.insert(sig).second) {
      // TEMP DIAG (MINIMAPRECT): the punch quad's own screen rectangle and UVs.
      //
      // Measured on the 220x220 target: alpha INSIDE the circle is 44.7 and
      // OUTSIDE 76.5 -- the punch subtracted where it should preserve and left
      // the corners untouched, which is what a quad that only covers the middle
      // does. Everything else about this draw already measured correct, so the
      // geometry is the last thing left unmeasured.
      {
        const uint32_t vbase = inline_geom ? inline_geom->address
                                           : (bound.streams.empty() ? 0u
                                                                    : bound.streams[0].guest_base);
        const uint32_t vstride = inline_geom ? inline_geom->stride
                                             : (bound.streams.empty() ? 0u
                                                                      : bound.streams[0].stride);
        const uint32_t vsize = inline_geom ? inline_geom->size_bytes
                                           : (bound.streams.empty() ? 0u
                                                                    : bound.streams[0].guest_size);
        uint32_t pos_off = 0xFFFFFFFFu, uv_off = 0xFFFFFFFFu;
        for (const auto& el : bound.input_layout) {
          if (!el.semantic_name || el.semantic_index != 0) continue;
          if (std::strcmp(el.semantic_name, "POSITION") == 0) pos_off = el.aligned_byte_offset;
          else if (std::strcmp(el.semantic_name, "TEXCOORD") == 0) uv_off = el.aligned_byte_offset;
        }
        // Inline geometry is virtual; a bound stream's base is PHYSICAL.
        const uint8_t* phys = inline_geom ? nullptr : TranslatePhysicalGuest(vbase);
        const bool ok = vbase && vstride && pos_off != 0xFFFFFFFFu &&
                        (inline_geom ? IsGuestRangeReadable(vbase, 6u * vstride)
                                     : (phys && IsPhysicalRangeReadable(vbase, 6u * vstride)));
        if (ok) {
          const auto rd = [&](uint32_t v, uint32_t o) {
            if (phys) {
              uint32_t d;
              std::memcpy(&d, phys + v * vstride + o, 4);
              return __builtin_bswap32(d);
            }
            return R32(base, vbase + v * vstride + o);
          };
          float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
          float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
          const uint32_t nv = std::min<uint32_t>(vsize / vstride, 6u);
          for (uint32_t v = 0; v < nv; ++v) {
            float fx, fy;
            const uint32_t rx = rd(v, pos_off), ry = rd(v, pos_off + 4);
            std::memcpy(&fx, &rx, 4);
            std::memcpy(&fy, &ry, 4);
            x0 = std::min(x0, fx); x1 = std::max(x1, fx);
            y0 = std::min(y0, fy); y1 = std::max(y1, fy);
            if (uv_off != 0xFFFFFFFFu && uv_off + 8 <= vstride) {
              float fu, fv;
              const uint32_t ru = rd(v, uv_off), rv = rd(v, uv_off + 4);
              std::memcpy(&fu, &ru, 4);
              std::memcpy(&fv, &rv, 4);
              u0 = std::min(u0, fu); u1 = std::max(u1, fu);
              v0 = std::min(v0, fv); v1 = std::max(v1, fv);
            }
          }
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f,
                         "MINIMAPRECT rgb_op=%u nv=%u rect=(%.1f,%.1f..%.1f,%.1f) "
                         "uv=(%.3f,%.3f..%.3f,%.3f) vp=(%.0f,%.0f %.0fx%.0f) target=%ux%u "
                         "stride=%u inline=%d\n",
                         blend_op_rgb, nv, x0, y0, x1, y1, u0, v0, u1, v1, hv.top_left_x,
                         hv.top_left_y, hv.width, hv.height, cfg.width, cfg.height, vstride,
                         inline_geom ? 1 : 0);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "MINIMAPDRAW #%u rgb_op=%u a_op=%u ctl=0x%08X premult_rgb=%u premult_a=%u "
                        "| depthctl=0x%08X stencil=%d ref=%u rd=0x%02X wr=0x%02X func=%u fail=%u pass=%u zfail=%u depth_test=%d depth_write=%d "
                        "alpha_test=%d thresh=%.6f ntex=%u |",
                     mm_index, blend_op_rgb, blend_op_a, rs.blend_control0,
                     shared_values.blend_premult_rgb, shared_values.blend_premult_a,
                     rs.depth_control, (rs.depth_control & 0x1u) ? 1 : 0, rs.stencil_ref,
                     rs.stencil_read_mask, rs.stencil_write_mask,
                     (rs.depth_control >> 8) & 0x7u, (rs.depth_control >> 11) & 0x7u,
                     (rs.depth_control >> 14) & 0x7u, (rs.depth_control >> 17) & 0x7u,
                     (rs.depth_control & 0x2u) ? 1 : 0, (rs.depth_control & 0x4u) ? 1 : 0,
                     rs.alpha_test_enable ? 1 : 0, double(shared_values.alpha_threshold),
                     bound_tex_count);
        for (uint32_t i = 0; i < bound_tex_count; ++i) {
          std::fprintf(f, " s%u=0x%08X %ux%u f%u %s", bound_tex[i].fetch_slot,
                       bound_tex[i].fetch.base_address, bound_tex[i].fetch.width,
                       bound_tex[i].fetch.height, bound_tex[i].fetch.format,
                       bound_tex[i].resolved ? "ok" : "FALLBACK");
        }
        std::fprintf(f, "\n");
        std::fflush(f);
        std::fclose(f);
      }
    }
  }

  ConstantBindings cbv;
  const auto t_const = ProfileClock::now();
  {
    D3D12Context::UploadAlloc a;
    // The upload ring is per frame slot and resets in BeginFrame, i.e. once per
    // BATCH. Exhausting it used to just drop the draw, which was worse than it
    // looks: draws_in_batch only advances for ACCEPTED draws, so a run of failed
    // draws never reached kDrawsPerBatch, the batch was never submitted, the
    // ring never reset, and every remaining draw of the frame failed the same
    // way. One exhaustion silently killed the rest of the frame.
    //
    // Submitting the batch here is what the original comment prescribed and what
    // was never implemented. The CURRENT draw is still lost -- its command list
    // is going out now and re-recording it mid-draw would mean replaying
    // viewport/RTV/PSO/barriers by hand -- but the next draw opens a fresh list
    // through EnsureFrame and re-establishes all of that anyway (every draw
    // already re-binds unconditionally), against an empty ring.
    const auto upload_or_flush = [&](uint64_t size) -> bool {
      if (context.AllocateUpload(size, 256, a, D3D12Context::UploadTag::kConstants)) {
        return true;
      }
      ++g_cap.fail_constants;
      ++g_cap.ring_flushes;
      FlushBatch(context);
      return false;
    };
    // Consecutive draws overwhelmingly share their constant banks -- the guest
    // flushes only the registers it changed, and most draws change none of the
    // 256. Re-uploading 8 KiB per draw into write-combined memory was both the
    // bandwidth and the reason the 16 MiB ring ran out every ~1800 draws, which
    // forced a batch submit. A mirror plus a memcmp reuses the previous
    // allocation whenever the bank is byte-identical.
    //
    // The reuse is only valid inside one batch: the ring is a bump allocator
    // that BeginFrame rewinds, so a GPU address from a previous batch points at
    // bytes that have since been overwritten. context.frame_index() advances on
    // every submit, which makes it exactly the right epoch.
    static std::vector<uint8_t> vs_mirror, ps_mirror;
    static uint64_t mirror_epoch = UINT64_MAX;
    static uint64_t mirror_vs_gpu = 0, mirror_ps_gpu = 0;
    const uint64_t epoch = context.frame_index();
    const bool epoch_ok = epoch == mirror_epoch && vs_mirror.size() == kAluBankBytes &&
                          ps_mirror.size() == kAluBankBytes;
    const bool vs_same =
        epoch_ok && std::memcmp(vs_mirror.data(), vs_bank.data(), kAluBankBytes) == 0;
    const bool ps_same =
        epoch_ok && std::memcmp(ps_mirror.data(), ps_bank.data(), kAluBankBytes) == 0;
    // Any early return below leaves the mirror describing an allocation this
    // draw abandoned, so drop it rather than hand it to the next draw.
    const auto invalidate_mirror = [&]() { mirror_epoch = UINT64_MAX; };

    if (vs_same) {
      cbv.vs = mirror_vs_gpu;
    } else {
      if (!upload_or_flush(kAluBankBytes)) {
        invalidate_mirror();
        return;
      }
      std::memcpy(a.cpu, vs_bank.data(), kAluBankBytes);
      cbv.vs = a.gpu;
    }
    if (ps_same) {
      cbv.ps = mirror_ps_gpu;
    } else {
      if (!upload_or_flush(kAluBankBytes)) {
        invalidate_mirror();
        return;
      }
      std::memcpy(a.cpu, ps_bank.data(), kAluBankBytes);
      cbv.ps = a.gpu;
    }
    if (!vs_same) {
      vs_mirror.assign(vs_bank.begin(), vs_bank.end());
    }
    if (!ps_same) {
      ps_mirror.assign(ps_bank.begin(), ps_bank.end());
    }
    mirror_vs_gpu = cbv.vs;
    mirror_ps_gpu = cbv.ps;
    mirror_epoch = epoch;

    // The shared buffer carries this draw's descriptor indices, so it is always
    // different and always uploaded.
    if (!upload_or_flush(kSharedConstantsBytes)) {
      invalidate_mirror();
      return;
    }
    std::memcpy(a.cpu, shared.data(), kSharedConstantsBytes);
    cbv.shared = a.gpu;
  }
  ProfileAdd(g_profile.const_us, t_const);

  const auto t_pso = ProfileClock::now();
  PsoKey key = PipelineCache::MakeKey(bound, rs, vs_id, ps_id, vs_spec, ps_spec);
  // A PSO whose sample count disagrees with the bound target is rejected
  // outright, so this has to follow the same rule the pool key does. Same for
  // the render-target count: a PSO declaring one target cannot be used with two
  // bound, and the guest's own oC1 write would be dropped.
  key.sample_count = PooledSampleCount(cfg);
  key.rt1_format = (REXCVAR_GET(mcla_native_gfx_mrt) & 0x20u) ? cfg.rt1_format : 0u;
  ID3D12PipelineState* pso = pipelines.GetOrCreate(context, key, vs_code, ps_code, bound);
  ProfileAdd(g_profile.pso_us, t_pso);
  if (!pso) {
    ++g_cap.fail_pso;
    CLOGF("%-5u %-6u %016llX %016llX %-8u PSO creation failed\n", g_cap.accepted, element_count,
          (unsigned long long)vs_id, (unsigned long long)ps_id, primitive_type);
    return;
  }

  // Reverse-Z shows up as an inverted viewport depth range (min > max); that
  // pass clears to 0 and tests GEQUAL. A normal range means the opposite, and
  // clearing it to 0 would reject every fragment.
  const float clear_depth = hv.min_depth > hv.max_depth ? 0.0f : 1.0f;
  // TEMP DIAG (remove after): remember the minimap target so the frame end can
  // dump it. Every previous read of this target was taken in a frame that did
  // NOT contain its draws, so it showed stale content -- which is why "the
  // punch does not erase the corners" was never actually measured.
  if (cfg.width == 220 && cfg.height == 220) {
    g_minimap_key = PooledKey(cfg);
    g_minimap_seen = true;
  }
  RenderTarget* target = render_targets.Acquire(context, PooledKey(cfg), clear_depth);
  // A draw that writes oC1 needs the pass's second surface attached before it
  // is bound. Kept out of the pool key on purpose -- see EnsureSecondTarget.
  bool have_second_target = false;
  if (target && cfg.rt1_format != 0) {
    have_second_target = render_targets.EnsureSecondTarget(context, *target, cfg.rt1_format);
  }
  // TEMP INSTRUMENTATION: accumulated per pass, not sampled from one draw.
  // Sampling the first draw was misleading: it is a depth prepass with the
  // colour mask at zero, which looks identical to a pass that never writes.
  NotePassDraw(cfg.width, cfg.height, cfg.rt_format, rs.color_mask);
  // TEMP DIAG (remove after): what the GUEST asks for in RB_SURFACE_INFO
  // against what the pool actually allocates. PooledSampleCountFields forces
  // the cvar count onto the HDR scene target and ONE sample everywhere else,
  // so any pass the guest multisamples and this line reports as pooled=1 is a
  // pass with no antialiasing on the native path.
  {
    static std::set<uint64_t> seen_msaa;
    // The pitch is part of the signature. Without it, two passes of the same
    // viewport shape but DIFFERENT surface widths collapse into one line, and
    // the second never prints -- which is how a 960x640 viewport into a
    // 1024-wide surface stayed invisible while its resolve missed 18 times.
    const uint64_t sig = (uint64_t(cfg.width) << 40) ^ (uint64_t(cfg.height) << 24) ^
                         (uint64_t(cfg.rt_format) << 12) ^ (uint64_t(rs.msaa_samples) << 8) ^
                         uint64_t(cfg.ds_format) ^ (uint64_t(rs.surface_info & 0x3FFFu) << 48) ^
                         (uint64_t(rs.color_info & 0xFFFu) << 4);
    if (seen_msaa.size() < 64 && seen_msaa.insert(sig).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        // RB_SURFACE_INFO bits 0..13 are the surface pitch in pixels: the width
        // of the EDRAM surface the pass renders into, which is NOT the viewport
        // width when the guest pads to a power of two. The pooled target is
        // sized from the viewport, so a resolve of the whole surface asks for a
        // shape no target has.
        std::fprintf(f,
                     "MSAA pass %ux%u rt=%u ds=%u guest_samples=%u pooled=%u pitch=%u "
                     "edram_base=%u colorinfo=0x%08X\n",
                     cfg.width, cfg.height, cfg.rt_format, cfg.ds_format,
                     1u << (rs.msaa_samples & 3u), PooledSampleCount(cfg),
                     rs.surface_info & 0x3FFFu, rs.color_info & 0xFFFu, rs.color_info);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  if (!target) {
    CLOGF("      no render target: %ux%u rt=%u ds=%u samples=%u\n", cfg.width, cfg.height,
          cfg.rt_format, cfg.ds_format, cfg.sample_count);
    // TEMP DIAG (BINDFAIL): which of the four fail_bind sites closes on the
    // frame, and for which target. With MSAA on the tally only said
    // "fail_bind", which is four different failures wearing one name.
    {
      static uint32_t n = 0;
      if (n++ < 24u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "BINDFAIL site=no_target %ux%u rt=%u ds=%u pooled_samples=%u\n",
                       cfg.width, cfg.height, cfg.rt_format, cfg.ds_format,
                       PooledSampleCount(cfg));
          std::fclose(f);
        }
      }
    }
    ++g_cap.fail_bind;
    return;
  }
  const bool is_aux = !(cfg == g_cap.config);
  // TEMP SKYPROBE: report the state of the sky-dome / mini-sky passes once each
  // so the reason the sky region stays at the clear colour can be pinned down
  // (depth-reject vs cull vs wrong target). Remove after the skybox is fixed.
  {
    static const uint64_t kSkyVs[] = {
        0x7E5802436ED6C520ull /*vs_main*/,     0xBA77A519EF33AFBDull /*vs_main_fast*/,
        0xB869978B200BFF5Bull /*vs_MiniSky*/,  0xAEAF86AD49C20C9Aull /*vs_BlurSky*/,
        0x892C3FF3002828BDull /*vs_stencil*/,  0x5A2CA13B2568F03Bull /*vs_DpBackMain*/,
        0x8E498B4ADDCDB58Bull /*atmoscatt vs_main*/};
    bool is_sky = false;
    for (uint64_t k : kSkyVs) {
      if (k == vs_id) {
        is_sky = true;
        break;
      }
    }
    if (is_sky) {
      static uint64_t seen[64] = {};
      static uint32_t seen_n = 0;
      const uint64_t tag = vs_id ^ ps_id ^ (uint64_t(cfg.width) << 40);
      bool fresh = true;
      for (uint32_t i = 0; i < seen_n; ++i) {
        if (seen[i] == tag) {
          fresh = false;
          break;
        }
      }
      if (fresh && seen_n < 64) {
        seen[seen_n++] = tag;
        REXLOG_INFO("[SKYPROBE] vs={:016X} ps={:016X} {}x{} anchor={} aux={} auxstage={} "
                    "depth_en={} depth_wr={} func={} cull_f={} cull_b={} cw={} cmask=0x{:X} "
                    "blend0=0x{:08X} vpz={:.3f}..{:.3f} cleardepth={:.1f} prim={} elems={}",
                    vs_id, ps_id, cfg.width, cfg.height, (cfg == g_cap.config) ? 1 : 0,
                    is_aux ? 1 : 0, aux_stage, rs.depth_enable ? 1 : 0, rs.depth_write ? 1 : 0,
                    rs.depth_func, rs.cull_front ? 1 : 0, rs.cull_back ? 1 : 0,
                    rs.front_face_is_cw ? 1 : 0, rs.color_mask, rs.blend_control0, hv.min_depth,
                    hv.max_depth, clear_depth, primitive_type, element_count);
        // Geometry/coverage side: if the dome collapses (bad position format /
        // stride) it generates no fragments even though depth would pass
        for (size_t si = 0; si < bound.streams.size(); ++si) {
          REXLOG_INFO("[SKYGEO]   stream{} stride={} guest_size={} endian={} indexed={} ibytes={}",
                      si, bound.streams[si].stride, bound.streams[si].guest_size,
                      bound.streams[si].endian, bound.indexed ? 1 : 0, bound.index_buffer_bytes);
        }
        for (const auto& el : bound.input_layout) {
          REXLOG_INFO("[SKYGEO]   attr {}[{}] fmt={} slot={} off={}",
                      el.semantic_name ? el.semantic_name : "?", el.semantic_index, el.dxgi_format,
                      el.input_slot, el.aligned_byte_offset);
        }
        // Inputs the dome samples: if a texture is unresolved (-> white
        // fallback) or the sky PS colour/exposure constants are zero, the dome
        // comes out flat/black regardless of geometry.
        REXLOG_INFO("[SKYTEX]   bound={} resolved={}", bound_tex_count, resolved_tex);
        for (uint32_t i = 0; i < bound_tex_count; ++i) {
          const BoundTexture& b = bound_tex[i];
          REXLOG_INFO("[SKYTEX]   slot={} resolved={} fmt={} {}x{} rt_sourced={}", b.fetch_slot,
                      b.resolved ? 1 : 0, b.fetch.format, b.fetch.width, b.fetch.height,
                      IsRenderTargetSourcedFormat(b.fetch.format) ? 1 : 0);
        }
        auto reg = [&](uint32_t r, uint32_t c) {
          float v;
          std::memcpy(&v, ps_bank.data() + r * 16 + c * 4, 4);
          return v;
        };
        REXLOG_INFO("[SKYCB]   c66(sun)={:.3f},{:.3f},{:.3f} c67(cloud)={:.3f},{:.3f},{:.3f} "
                    "c79(hdrexp)={:.3f} c81(hdrclamp)={:.3f},{:.3f},{:.3f}",
                    reg(66, 0), reg(66, 1), reg(66, 2), reg(67, 0), reg(67, 1), reg(67, 2),
                    reg(79, 0), reg(81, 0), reg(81, 1), reg(81, 2));
      }
    }
  }
  if (is_aux && aux_stage < 2) {
    // Stage 1: the target was created; record nothing with it.
    if (is_display) ++g_cap.accepted_composite; else ++g_cap.accepted_aux;
    return;
  }
  render_targets.FlushPendingCopies(context, cl);
  render_targets.FlushPendingTransitions(cl);
  // A draw that SAMPLES the depth buffer it renders with -- the distance-fog
  // pass reads the scene depth to turn it into a distance -- cannot have that
  // resource in DEPTH_WRITE, and D3D12 has no state that is both writable and
  // shader-readable. Give it the read-only depth view and the matching state.
  bool samples_own_depth = false;
  for (uint32_t i = 0; i < bound_tex_count; ++i) {
    if (target->depth && bound_tex[i].resource == target->depth.Get()) {
      samples_own_depth = true;
      break;
    }
  }
  render_targets.PrepareForRendering(cl, *target, samples_own_depth);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = target->rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = target->dsv_heap->GetCPUDescriptorHandleForHeapStart();
  if (samples_own_depth) {
    dsv.ptr += target->dsv_descriptor_size;
  }
  // Both colour targets when the pass declares two. The RTVs are slots 0 and 1
  // of the same heap, so a single handle plus RTsSingleHandleToDescriptorRange
  // covers them.
  const UINT rtv_count =
      (have_second_target && (REXCVAR_GET(mcla_native_gfx_mrt) & 0x20u)) ? 2u : 1u;
  cl->OMSetRenderTargets(rtv_count, &rtv, rtv_count > 1 ? TRUE : FALSE, &dsv);
  if (target->cleared && target->needs_depth_reclear) {
    // A full-source depth resolve since the last draw ended this surface's
    // pass. The shadow map reuses ONE 640x640 surface for four cascades and
    // resolves each into its own atlas quadrant, so "cleared once per frame"
    // made cascades 1..3 render against cascade 0's depth. Depth only: the
    // colour latch below is a different rule with a different blast radius.
    target->needs_depth_reclear = false;
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              target->clear_depth, 0, 0, nullptr);
  }
  // Consume the guest's pending clear ONCE, here, so both branches below see
  // the same request: the first-draw policy clear and the mid-frame re-clear.
  GuestClearRequest guest_clear;
  const bool have_guest_clear =
      REXCVAR_GET(mcla_native_gfx_guest_clear) && TakeGuestClear(&guest_clear);
  if (!target->cleared) {
    // Cleared once per target: every later draw of that pass accumulates into
    // it, which is what makes the shadow atlas and the scene build up.
    target->cleared = true;
    target->needs_depth_reclear = false;
    // The guest's own clear for this pass never reaches the runtime -- it goes
    // to EDRAM through the command processor, which no-CP mode does not run --
    // so kClearColor stands in for it. That is wrong wherever the pass does not
    // cover the whole surface: the ShadowBlend target keeps the debug colour
    // over the 80% of the screen no ShadowBlend draw touches, and that reads as
    // full shadow. TakeGuestClear hands back the colour D3DDevice_Clear was
    // called with instead. See mcla_native_gfx_guest_clear.
    float clear_rgba[4] = {kClearColor[0], kClearColor[1], kClearColor[2], kClearColor[3]};
    const bool from_guest = have_guest_clear && guest_clear.color;
    if (from_guest) {
      std::memcpy(clear_rgba, guest_clear.rgba, sizeof(clear_rgba));
    }
    (void)from_guest;
    cl->ClearRenderTargetView(rtv, clear_rgba, 0, nullptr);
    if (target->color1 && (REXCVAR_GET(mcla_native_gfx_mrt) & 0x4u)) {
      // Target 1 is cleared with target 0's colour: the guest issues one
      // D3DDevice_Clear for the pass, and on the console it wipes every bound
      // surface. Leaving it dirty would let the previous species' normals show
      // through the holes in this one's silhouette, which is the same class of
      // bug the impostor atlas re-clear fixed for colour.
      D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = rtv;
      rtv1.ptr += target->rtv_descriptor_size;
      cl->ClearRenderTargetView(rtv1, clear_rgba, 0, nullptr);
    }
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              target->clear_depth, 0, 0, nullptr);
  } else if (have_guest_clear && REXCVAR_GET(mcla_native_gfx_reclear)) {
    // One pooled target serves every pass of the same shape, so "cleared once
    // per frame" hands the second pass of a frame the first pass's pixels. The
    // guest clears before each pass; honour that clear here, restricted to the
    // pass's own viewport, which is what D3DDevice_Clear does on the console.
    // Scissoring is what makes this safe for the shadow atlas, whose quadrants
    // are separate passes into one surface: each clear only wipes its quadrant.
    const D3D12_RECT rect = {LONG(hv.top_left_x), LONG(hv.top_left_y),
                             LONG(hv.top_left_x + hv.width),
                             LONG(hv.top_left_y + hv.height)};
    if (rect.right > rect.left && rect.bottom > rect.top && guest_clear.color) {
      cl->ClearRenderTargetView(rtv, guest_clear.rgba, 1, &rect);
      if (target->color1 && (REXCVAR_GET(mcla_native_gfx_mrt) & 0x4u)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = rtv;
        rtv1.ptr += target->rtv_descriptor_size;
        cl->ClearRenderTargetView(rtv1, guest_clear.rgba, 1, &rect);
      }
    }
    // Colour only. Depth already has its own rule, needs_depth_reclear above,
    // driven by a full-source resolve -- which is the exact moment a pass ends.
    // Clearing depth here as well fired mid-pass on the anchor and the shadow
    // atlas (measured: 9 and 27 full-surface depth clears per capture) for no
    // gain, since the resolve-driven rule had already covered both.
  }

  // TEMP DIAG (KEYMISMATCH): the pool's key against the resource it actually
  // holds. The scissor comes from the key and the RTV from the resource, so if
  // the two ever disagree a draw rasterises through one rectangle into a
  // surface of another size -- measured once on the pause menu composite, whose
  // scissor read 960x640 while the bound RTV was the 1280x720 composite.
  if (target->color) {
    const D3D12_RESOURCE_DESC rd = target->color->GetDesc();
    if (uint32_t(rd.Width) != target->key.width || rd.Height != target->key.height) {
      static uint32_t n = 0;
      if (n++ < 16u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "KEYMISMATCH key=%ux%u res=%llux%u fmt=%u vp=%.0fx%.0f ps=%016llX\n",
                       target->key.width, target->key.height,
                       (unsigned long long)rd.Width, rd.Height, unsigned(rd.Format), hv.width,
                       hv.height, (unsigned long long)ps_id);
          std::fflush(f); std::fclose(f);
        }
      }
    }
  }
  D3D12_VIEWPORT vp = {hv.top_left_x, hv.top_left_y, hv.width,
                       hv.height,     hv.min_depth,  hv.max_depth};
  // Scissor from THIS draw's target, not from the anchor's. Using the anchor
  // size set a 1280x720 scissor while rendering into a 256x256 auxiliary
  // target, i.e. a scissor larger than the render target — which is what hung
  // the GPU as soon as any pass other than the anchor was rendered
  // (DXGI_ERROR_DEVICE_HUNG, always on a pass of a different size).
  D3D12_RECT sc = {0, 0, LONG(target->key.width), LONG(target->key.height)};
  cl->RSSetViewports(1, &vp);
  cl->RSSetScissorRects(1, &sc);
  // TEMP DIAG (remove after): the viewport a draw actually rasterises through,
  // once per distinct rect. ComputeHostViewport never validates that the rect
  // lands inside its own target, and `y_flipped` is compensated only as a
  // winding change in the PSO -- never geometrically -- so a fullscreen quad
  // can be recorded, counted, and still rasterise nowhere visible.
  {
    static std::set<uint64_t> seen_vp;
    const uint64_t sig = (uint64_t(uint32_t(vp.TopLeftX)) << 44) ^
                         (uint64_t(uint32_t(vp.TopLeftY)) << 32) ^
                         (uint64_t(uint32_t(vp.Width)) << 20) ^
                         (uint64_t(uint32_t(vp.Height)) << 8) ^
                         (uint64_t(target->key.width) << 4) ^ uint64_t(hv.y_flipped ? 1 : 0);
    if (seen_vp.insert(sig).second) {
      if (FILE* fv = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(fv,
                     "VIEWPORT x=%.1f y=%.1f w=%.1f h=%.1f depth=[%.3f,%.3f] yflip=%d | "
                     "target %ux%u rt=%u ds=%u | scissor %ldx%ld\n",
                     vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
                     hv.y_flipped ? 1 : 0, target->key.width, target->key.height,
                     target->key.rt_format, target->key.ds_format, long(sc.right),
                     long(sc.bottom));
        std::fflush(fv);
        std::fclose(fv);
      }
    }
  }

  ID3D12DescriptorHeap* heaps[] = {binder.srv_heap(), binder.sampler_heap()};
  cl->SetDescriptorHeaps(2, heaps);
  cl->SetGraphicsRootSignature(pipelines.root_signature());
  cl->SetPipelineState(pso);
  cl->OMSetStencilRef(rs.stencil_ref);
  cl->OMSetBlendFactor(rs.blend_constant);
  cl->SetGraphicsRootConstantBufferView(kRootVsConstants, cbv.vs);
  cl->SetGraphicsRootConstantBufferView(kRootPsConstants, cbv.ps);
  cl->SetGraphicsRootConstantBufferView(kRootSharedConstants, cbv.shared);
  const D3D12_GPU_DESCRIPTOR_HANDLE srv_base =
      binder.srv_heap()->GetGPUDescriptorHandleForHeapStart();
  cl->SetGraphicsRootDescriptorTable(kRootTexture2DTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootTexture3DTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootTextureCubeTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootSamplerTable,
                                     binder.sampler_heap()->GetGPUDescriptorHandleForHeapStart());

  // TEMP INSTRUMENTATION: the shadow pass is the only one with ds_format 45.
  if (cfg.ds_format == 45 && g_shadow_record_count < kMaxShadowRecords) {
    ShadowDrawRecord& r = g_shadow_records[g_shadow_record_count];
    r.index = g_shadow_record_count;
    ++g_shadow_record_count;
    r.element_count = element_count;
    r.start_element = start_element;
    r.base_vertex = base_vertex;
    r.primitive_type = primitive_type;
    r.indexed = indexed;
    r.vs_id = vs_id;
    r.ps_id = ps_id;
    r.vp_x = hv.top_left_x;
    r.vp_y = hv.top_left_y;
    r.vp_w = hv.width;
    r.vp_h = hv.height;
    r.vp_min_z = hv.min_depth;
    r.vp_max_z = hv.max_depth;
    r.y_flipped = hv.y_flipped;
    r.scissor_w = target->key.width;
    r.scissor_h = target->key.height;
    r.pa_su_sc_mode_cntl = rs.pa_su_sc_mode_cntl;
    r.depth_control = rs.depth_control;
    r.color_mask = rs.color_mask;
    r.blend_control0 = rs.blend_control0;
    r.color_control = rs.color_control;
    r.mode_control = rs.mode_control;
    r.cull_front = rs.cull_front;
    r.cull_back = rs.cull_back;
    r.front_face_is_cw = rs.front_face_is_cw;
    r.depth_enable = rs.depth_enable;
    r.depth_write = rs.depth_write;
    r.stencil_enable = rs.stencil_enable;
    r.depth_func = rs.depth_func;
    r.rt_format = cfg.rt_format;
    r.ds_format = cfg.ds_format;
    r.sample_count = cfg.sample_count;
    r.clear_depth = target->clear_depth;
    r.stream_count = uint32_t(bound.streams.size());
    r.layout_elements = uint32_t(bound.input_layout.size());
    if (!bound.streams.empty()) {
      r.stride0 = bound.streams[0].stride;
      r.base0 = bound.streams[0].guest_base;
      r.size0 = bound.streams[0].guest_size;
      r.endian0 = bound.streams[0].endian;
    }
    r.index_32bit = bound.index_32bit;
    r.index_bytes = bound.index_buffer_bytes;
    r.pso_key_hash = uint64_t(PsoKeyHash{}(key));
    std::memcpy(r.vs_c, vs_bank.data(), sizeof(r.vs_c));
  }

  // Reject a draw whose element_count is corrupt before it reaches the GPU: an
  // absurdly large count (no MCLA draw approaches millions of elements) or, for
  // an indexed draw, one that runs past its own index buffer. Such a draw makes
  // the GPU process an enormous/garbage range and spins it for seconds -> TDR
  // (DXGI_ERROR_DEVICE_HUNG 0x887A0006) -> the SDK's host-GPU-loss handler
  // aborts. This only began firing once the resolve-format fix stopped the
  // capture dying mid-frame, so heavy frames are now reached and re-rendered.
  {
    constexpr uint32_t kSaneMaxElements = 3u * 1024u * 1024u;
    bool bad = element_count > kSaneMaxElements;
    if (!bad && bound.indexed && !expand_topology) {
      const uint32_t isz = bound.index_32bit ? 4u : 2u;
      const uint32_t max_idx = bound.index_buffer_bytes / isz;
      bad = (max_idx == 0) || (uint64_t(start_element) + element_count > uint64_t(max_idx));
    }
    // A non-indexed draw reads vertices [start, start+count) straight from the
    // buffer; when that runs past the bound vertex buffer the D3D12 IA returns
    // zero for the out-of-range vertices, which places them at the origin and
    // draws long thin triangles/lines to a screen corner (observed as stray
    // black lines with start_element far beyond the buffer). Reject it.
    if (!bad && !bound.indexed && !expand_topology && !bound.streams.empty()) {
      const uint32_t stride = bound.streams[0].stride;
      const uint32_t vcount = stride ? bound.streams[0].guest_size / stride : 0;
      bad = (vcount == 0) || (uint64_t(start_element) + element_count > uint64_t(vcount));
    }
    if (bad) {
      static unsigned n = 0;
      if (n++ < 16) {
        REXLOG_ERROR("[native_gfx] skip pathological draw: prim={} start={} count={} indexed={} "
                     "ib_bytes={} vs={:016X} ps={:016X}",
                     primitive_type, start_element, element_count, bound.indexed ? 1 : 0,
                     bound.index_buffer_bytes, (unsigned long long)vs_id, (unsigned long long)ps_id);
      }
      ++g_cap.rej_config;
      ++g_cap.rej_cfg_patho;
      if (++g_cap.draws_in_batch >= kDrawsPerBatch) {
        FlushBatch(context);
      }
      return;
    }
  }

  TopologyExpander::Buffer expansion;
  if (expand_topology) {
    expansion = g_topology.Acquire(context, primitive_type, element_count);
    if (expansion.index_count == 0) {
      ++g_cap.rej_topology;
      return;
    }
    // The PSO already asks for D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: the
    // switch in PipelineCache falls through to it for every type without a
    // direct topology, so the expanded draw needs no separate pipeline.
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  } else {
    cl->IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY(PrimitiveTypeToTopology(primitive_type)));
  }
  std::vector<D3D12_VERTEX_BUFFER_VIEW> vbvs;
  for (const VertexStream& stream : bound.streams) {
    D3D12_VERTEX_BUFFER_VIEW v = {};
    v.BufferLocation = stream.gpu_address;
    v.SizeInBytes = stream.guest_size;
    v.StrideInBytes = stream.stride;
    vbvs.push_back(v);
  }
  // Rebuilding the vertex data for a shader that computes its own fetch index.
  //
  // On the Xenos a vfetch takes its index from a REGISTER, and r0.x arrives at
  // the vertex shader preloaded with the vertex index (rexglue's own translator
  // does exactly that, DxbcShaderTranslator::StartVertexShader_LoadVertexIndex).
  // Four of MCLA's shaders exploit it to expand a point into a quad:
  //   corner      = r0.x % 4        -> one of uvs[4], a quad's UV/sign pairs
  //   fetch index = trunc(r0.x / 4) -> which prop this quad belongs to
  // so the hardware runs four vertices per source vertex and the shader folds
  // them back onto one. HasComputedVertexFetchIndex finds them in the microcode
  // -- xPropFoliageImpostor x3 and xrain_system__ParticleRenderVS, and no
  // others out of 1279 vertex shaders.
  //
  // XenosRecomp cannot express that: recompile(VertexFetchInstruction) discards
  // instr.srcRegister and emits a plain input-assembler attribute, fetched at
  // the IA index. So the four corners of a quad read four DIFFERENT props and
  // the quad spans all four -- the black shards. XenosRecomp now declares
  // SV_VertexID and seeds r0.x with it, which restores the corner; this
  // restores the fetch, by putting source vertex i/4 at entry i.
  //
  // Measured on mapblacklines.rdc by replaying the shader's own arithmetic
  // against the captured constants (transcription verified against the captured
  // post-VS output to 0.0011 in ~9000): 1518 draws in one frame use
  // VSPropInstanceFoliage, 70081 quads, of which 7936 are wider than a tenth of
  // the screen and 412 read past the bound view. Under this rebuild: zero and
  // zero, every quad between 0.0045 and 0.031 NDC.
  //
  // Keyed on the shader, not on the shape of the draw. An earlier version
  // triggered on the fetch running off the end of the view, which is what the
  // menu capture showed; the map capture then showed the same shader with room
  // to spare (EID 37128: 56 vertices asked of a 454-vertex view), reading real
  // but wrong vertices instead of overrunning. Only the microcode says which
  // draws need this.
  if (expand_topology && vs_folds_fetch_index && primitive_type == 13 /* kQuadList */ &&
      !inline_geom && !bound.streams.empty() && vbvs.size() == bound.streams.size()) {
    // Four vertices per quad is the hardware relationship, and it agrees with
    // what all four shaders actually compute (each multiplies the index by
    // 0.25). Other expandable primitives are left alone and counted.
    const uint32_t sources = (element_count + 3u) / 4u;
    // The expanded draw is issued as DrawIndexedInstanced(..., StartIndex 0)
    // over zero-based generated indices, so entry i of the rebuilt stream has
    // to already hold what guest vertex start_element + i would have fetched.
    // Dropping start_element made every one of these draws read the same head
    // of the buffer. Measured in "cap_city2.rdc": 211 foliage impostor draws,
    // 220 on-screen vertices out of ~14000 sampled, every draw landing in the
    // same off-screen band at ndc x in [0.8, 2.1], y ~ 1.0 -- one wrong cluster
    // of trees drawn 211 times instead of the city's trees. The positions
    // themselves were sane and the matrix was byte-identical to the city
    // draws', which is what ruled out the shader, the fold and the constants.
    //
    // This is NOT the wider "fold start_element into BaseVertexLocation" change
    // the counter below tracks: that one moves every expanded draw. The fold
    // builds its own vertex buffer, so it is the one place that can take the
    // right slice without touching anything else.
    const uint32_t first_source = start_element / 4u;
    const uint32_t last_source =
        element_count != 0 ? (start_element + element_count - 1u) / 4u : first_source;
    bool rebuilt = sources != 0;
    uint32_t rebuilt_streams = 0;
    for (size_t i = 0; rebuilt && i < bound.streams.size(); ++i) {
      const VertexStream& st = bound.streams[i];
      // A zero-fill stream stands in for an attribute the declaration does not
      // supply: stride 0, every vertex reading the same zeros off the shared
      // zero buffer. Folding it is meaningless and it has no guest memory to
      // read, so it is skipped, not failed. Failing on it is what made this
      // rebuild report 0 successes out of 130 attempts per frame -- these draws
      // very nearly always carry one.
      if (st.zero_fill || st.stride == 0) {
        continue;
      }
      const uint64_t src_bytes = (uint64_t(last_source) + 1u) * st.stride;
      if (src_bytes > st.guest_size) {
        ++g_cap.quad_fail_size;
        if (g_cap.quad_fail_size <= 4) {
          REXLOG_WARN(
              "[native_gfx] fold: source range past fetch size -- elements={} sources={} "
              "stride={} need={} guest_size={} base={:#010x} streams={}",
              element_count, sources, st.stride, uint32_t(src_bytes), st.guest_size, st.guest_base,
              uint32_t(bound.streams.size()));
        }
        rebuilt = false;
        break;
      }
      // Vertex streams are PHYSICAL addresses out of the fetch constant, not
      // guest virtual ones: reading them through the virtual membase lands on
      // unmapped pages (see TranslatePhysicalGuest in guest_resources.h). Using
      // the virtual pair here is what made every one of ~184 rebuild attempts
      // per frame fail the readability check.
      const uint8_t* src = TranslatePhysicalGuest(st.guest_base);
      if (!src || !IsPhysicalRangeReadable(st.guest_base, src_bytes)) {
        ++g_cap.quad_fail_read;
        rebuilt = false;
        break;
      }
      const uint64_t bytes = uint64_t(element_count) * st.stride;
      D3D12Context::UploadAlloc alloc;
      if (!context.AllocateUpload(bytes, 16, alloc, D3D12Context::UploadTag::kGeometry)) {
        ++g_cap.quad_fail_alloc;
        rebuilt = false;
        break;
      }
      const BufferSwap swap = st.endian == 2   ? BufferSwap::k8in32
                              : st.endian == 1 ? BufferSwap::k8in16
                                               : BufferSwap::kNone;
      uint8_t* dst = static_cast<uint8_t*>(alloc.cpu);
      for (uint32_t v = 0; v < element_count; ++v) {
        SwapCopyBytes(dst + uint64_t(v) * st.stride,
                      src + (uint64_t(start_element) + v) / 4u * st.stride, st.stride, swap);
      }
      vbvs[i].BufferLocation = alloc.gpu;
      vbvs[i].SizeInBytes = UINT(bytes);
      vbvs[i].StrideInBytes = st.stride;
      ++rebuilt_streams;
    }
    if (rebuilt && rebuilt_streams == 0) {
      ++g_cap.quad_fail_nostream;
      rebuilt = false;
    }
    if (rebuilt) {
      ++g_cap.quad_replicated;
    } else {
      ++g_cap.quad_replicate_failed;
    }
    // TEMP DIAG (FOLDDIAG): whether start_element is actually non-zero for
    // these draws is the whole premise of taking it into account above, and the
    // capture cannot show it.
    {
      static uint32_t fold_lines = 0;
      if (fold_lines++ < 48u) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f,
                       "FOLDDIAG rebuilt=%d elements=%u start=%u sources=%u first=%u last=%u "
                       "streams=%u base=0x%08X stride=%u size=%u\n",
                       rebuilt ? 1 : 0, element_count, start_element, sources, first_source,
                       last_source, rebuilt_streams,
                       bound.streams.empty() ? 0u : bound.streams[0].guest_base,
                       bound.streams.empty() ? 0u : bound.streams[0].stride,
                       bound.streams.empty() ? 0u : bound.streams[0].guest_size);
          std::fclose(f);
        }
      }
    }
  }
  // An expanded draw is non-indexed, so the guest's first vertex is
  // start_element -- and the expanded path drops it, because the generated
  // indices are zero-based and D3D12's indexed draw has no start-vertex
  // parameter. Folding it into BaseVertexLocation was tried and shipped
  // together with two other changes, and the frame got worse; it is not going
  // back in on a guess. This counts how often it could matter at all.
  if (expand_topology && start_element != 0) {
    ++g_cap.expanded_nonzero_start;
  }
  cl->IASetVertexBuffers(0, UINT(vbvs.size()), vbvs.data());
  // A draw the GPU cannot satisfy: the expanded index count needs more vertices
  // than the bound view holds. D3D12 returns ZERO for an out-of-bounds vertex
  // fetch, so those vertices land at the origin and drag long thin triangles
  // across the frame -- the black shards over the map.
  //
  // Measured in blacklines.rdc: 22 draws where the expanded index count is
  // exactly 6x the vertices the view holds, at stride 32. Quad expansion is
  // (vertex_count / 4) * 6, so a ratio of 6 means the expansion was fed four
  // times the vertices the view covers. The same 4.0-exact ratio was seen once
  // before and written off as coincidence; across sizes 3, 11, 20, 37, 38, 39
  // and 101 it is not.
  //
  // The capture only shows the result. This prints the inputs -- which
  // primitive, how many vertices the guest asked for, how big the view is and
  // where it came from -- which is what separates "the view is too small" from
  // "the vertex count is too large".
  if (expand_topology && !vbvs.empty() && vbvs[0].StrideInBytes) {
    const uint32_t view_verts = vbvs[0].SizeInBytes / vbvs[0].StrideInBytes;
    const uint32_t expanded_indices = expansion.size_bytes / 4u;
    if (view_verts && expanded_indices > view_verts * 3u) {
      // Do not record the draw. Every piece of guest state agrees here --
      // stride 32 and its patcher mirror, fetch slot 95 - stream, the size
      // field as a dword count (proved by A/B: reading it as 16-byte units
      // erases half the map) -- and the guest memory past the fetch is
      // garbage, so the vertices this draw asks for genuinely do not exist.
      //
      // What does exist is exactly one quarter of them: element_count is 4x the
      // vertices the fetch covers, on every one of these draws. kQuadList
      // consumes four vertices per quad, so the guest is describing 37 quads
      // built from 37 vertices -- one vertex per quad, with the Xenos fetching
      // vertex = index/4 and using index%4 to pick the corner. The vfetch
      // instruction takes its index from a REGISTER the shader computes.
      //
      // XenosRecomp cannot express that: it turns vfetch into a declared input
      // attribute fetched by the input-assembler index, and emits SV_VertexID
      // only under UNLEASHED_RECOMP. So the shader has no index to divide, and
      // binding the buffer by IA index makes corner 1..3 of every quad read off
      // the end -- D3D12 returns zero, the corner lands at the world origin,
      // and one enormous black shard is drawn per quad.
      //
      // Dropping loses a small overlay effect. Drawing paints shards across the
      // whole map. Until the shader can compute its own fetch index, not
      // drawing is the honest option, and the counter says what it costs.
      // NOT rejected any more. Dropping these was tried and measured: 10375
      // draws per report left the frame and the shards stayed exactly as they
      // were, so they were never the source -- and losing that much real
      // geometry is worse than the artefact. The counter and the one-line
      // report stay: the 4x relationship they measure is real and still
      // unexplained. The amputation does not.
      ++g_cap.rej_outruns_fetch;
      static std::set<uint64_t> seen_short;
      const uint64_t sig = (uint64_t(primitive_type) << 48) ^ (uint64_t(view_verts) << 24) ^
                           uint64_t(expanded_indices);
      if (seen_short.size() < 32 && seen_short.insert(sig).second) {
        REXLOG_WARN(
            "[native_gfx] expanded draw outruns its vertex view: prim={} vertex_count={} "
            "expanded_indices={} view_bytes={} stride={} view_verts={} ratio={:.1f} "
            "inline={} guest_base={:#010x} fetch0={:#010x} fetch1={:#010x} need_bytes={} "
            "decl_stream={} stride_byte={} mirror_byte={} streams={} attrs=[{}] pos:{}",
            primitive_type, element_count, expanded_indices, vbvs[0].SizeInBytes,
            vbvs[0].StrideInBytes, view_verts, double(expanded_indices) / double(view_verts),
            inline_geom ? 1 : 0, bound.streams.empty() ? 0u : bound.streams[0].guest_base,
            bound.streams.empty() ? 0u : bound.streams[0].fetch_dword0,
            bound.streams.empty() ? 0u : bound.streams[0].fetch_dword1,
            element_count * vbvs[0].StrideInBytes,
            bound.streams.empty() ? 0u : bound.streams[0].decl_stream,
            bound.streams.empty() ? 0u : bound.streams[0].stride_table_byte,
            bound.streams.empty() ? 0u : bound.streams[0].stride_mirror_byte,
            uint32_t(bound.streams.size()), [&] {
              // Where the attributes actually END inside the vertex. If nothing
              // reaches past byte 8 the data really is 8 bytes per vertex and a
              // 32-byte stride is four times too wide; if they spread to ~32 the
              // stride is right and the primitive's vertex count is what is
              // being misread.
              std::string a;
              char one[48];
              for (const InputElement& e : bound.input_layout) {
                std::snprintf(one, sizeof(one), "%s%s%u@%u:fmt%u", a.empty() ? "" : " ",
                              e.semantic_name ? e.semantic_name : "?", e.semantic_index,
                              e.aligned_byte_offset, e.dxgi_format);
                a += one;
              }
              return a;
            }(),
            [&] {
              // The guest data itself, past where the fetch constant says the
              // buffer ends. Every piece of DEVICE state now agrees -- stride
              // 32, mirror 32, slot 95, size unit 4 -- and they cannot all be
              // right while a 148-vertex draw has a 37-vertex fetch. So the
              // question is no longer what the state says but what is actually
              // in memory: if the vertices the draw asks for are there, the
              // fetch size is the thing lying; if they are zeros, the guest
              // really did ask for vertices that do not exist and the console
              // must be discarding them somewhere this runtime is not.
              if (bound.streams.empty()) {
                return std::string("(no stream)");
              }
              const VertexStream& st = bound.streams[0];
              std::string a;
              char one[80];
              for (uint32_t v : {0u, view_verts, element_count - 1u}) {
                const uint32_t ea = st.guest_base + v * st.stride;
                if (!IsPhysicalRangeReadable(ea, 12)) {
                  std::snprintf(one, sizeof(one), " v%u=UNREADABLE", v);
                  a += one;
                  continue;
                }
                const uint8_t* p = TranslatePhysicalGuest(ea);
                float f[3] = {};
                for (uint32_t i = 0; i < 3; ++i) {
                  uint32_t w;
                  std::memcpy(&w, p + i * 4, 4);
                  w = __builtin_bswap32(w);
                  std::memcpy(&f[i], &w, 4);
                }
                std::snprintf(one, sizeof(one), " v%u=(%.2f,%.2f,%.2f)", v, f[0], f[1], f[2]);
                a += one;
              }
              return a;
            }());
      }
    }
  }
  // A non-indexed draw has no index buffer to describe. Binding a zeroed view
  // would leave a stale one from the previous draw bound instead.
  if (expand_topology) {
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = expansion.gpu_address;
    ibv.SizeInBytes = expansion.size_bytes;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    cl->IASetIndexBuffer(&ibv);
  } else if (bound.indexed) {
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = bound.index_gpu_address;
    ibv.SizeInBytes = bound.index_buffer_bytes;
    ibv.Format = bound.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    cl->IASetIndexBuffer(&ibv);
  } else {
    cl->IASetIndexBuffer(nullptr);
  }

  if (is_aux && aux_stage < 3) {
    // Stage 2: bound and cleared, but no draw recorded.
    if (is_display) ++g_cap.accepted_composite; else ++g_cap.accepted_aux;
    if (++g_cap.draws_in_batch >= kDrawsPerBatch) {
      FlushBatch(context);
    }
    return;
  }
  // Identity dump for a range of per-frame draw numbers, written next to the
  // hangfind file. Once bisection has narrowed the artefact to a few hundred
  // draws, this says what those draws actually ARE without another build.
  //
  // Deliberately the same fields the hangfind path prints, because those are
  // the ones that have explained real artefacts here before: the stream strides
  // and guest bases (a wrong stride reads the wrong buffer), the world-view-
  // projection (a degenerate transform is what drags a triangle across the
  // screen), and the bound textures.
  {
    const uint32_t dump_last = REXCVAR_GET(mcla_native_gfx_dump_draw_last);
    if (dump_last != 0 && g_cap.offered >= REXCVAR_GET(mcla_native_gfx_dump_draw_first) &&
        g_cap.offered <= dump_last) {
      if (FILE* f = std::fopen("native_gfx_draws.txt", "ab")) {
        std::fprintf(f,
                     "draw#%u vs=%016llX ps=%016llX prim=%u elements=%u expand=%d is_aux=%d "
                     "is_display=%d target=%ux%u rt_format=%u indexed=%d base_vertex=%d\n",
                     g_cap.offered, (unsigned long long)vs_id, (unsigned long long)ps_id,
                     primitive_type, element_count, expand_topology ? 1 : 0, is_aux ? 1 : 0,
                     is_display ? 1 : 0, cfg.width, cfg.height, cfg.rt_format,
                     bound.indexed ? 1 : 0, bound.base_vertex);
        for (const auto& st : bound.streams) {
          std::fprintf(f, "  stream slot=%u guest_base=0x%08X stride=%u size=%u\n", st.fetch_slot,
                       st.guest_base, st.stride, st.guest_size);
        }
        const float* c = reinterpret_cast<const float*>(vs_bank.data() + 8 * 16);
        std::fprintf(f, "  WVP:");
        for (int i = 0; i < 16; ++i) std::fprintf(f, " %.4g", c[i]);
        std::fprintf(f, "\n");
        std::fclose(f);
      }
    }
  }

  // TEMP DIAG (PUNCHISSUED): does the minimap punch reach a draw call at all?
  //
  // MINIMAPDRAW prints right after BuildGeometrySnapshot, hundreds of lines
  // before this point, so it proves the draw was OFFERED and nothing more. The
  // 220x220 target's alpha is the road network across the whole square with no
  // trace of a circle, while the punch's mask, blend, shader and geometry all
  // measured correct -- so the question is whether the draw is issued.
  if (cfg.width == 220u && cfg.height == 220u) {
    static uint64_t punches = 0, others = 0;
    static uint64_t punch_ps = 0, punch_vs = 0;
    static uint32_t punch_tex = 0, punch_texw = 0, punch_texh = 0, punch_elems = 0;
    static uint32_t punch_shared0 = 0, punch_srv = 0, punch_slot = 0, punch_ntex = 0;
    const bool is_punch = rs.blend_control0 == 0x01810181u;
    if (is_punch) {
      ++punches;
      punch_ps = ps_id;
      punch_vs = vs_id;
      punch_elems = element_count;
      punch_ntex = bound_tex_count;
      // What the SHADER reads: SharedConstants[0] is the Texture2D descriptor
      // index for fetch slot 0, which is where xAlphaModulate's MaskSampler
      // lives (packoffset(c0.x)). If it disagrees with the descriptor the
      // runtime says it wrote for the mask, the shader is sampling a different
      // resource than every diagnostic here reports -- and a uniform sample is
      // exactly what the measurements show (alpha 105.9 inside vs 96.7 outside
      // immediately after the punch, i.e. no circle at all).
      std::memcpy(&punch_shared0, shared.data(), 4);
      if (bound_tex_count) {
        punch_tex = bound_tex[0].fetch.base_address;
        punch_texw = bound_tex[0].fetch.width;
        punch_texh = bound_tex[0].fetch.height;
        punch_srv = bound_tex[0].srv_descriptor_index;
        punch_slot = bound_tex[0].fetch_slot;
      }
    } else {
      ++others;
    }
    static uint64_t tick = 0;
    if ((++tick % 2000u) == 0u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "PUNCHISSUED punch=%llu outros=%llu | expand=%d prim=%u idx=%u "
                        "color_mask=0x%X ctl=0x%08X rt=%p rtfmt=%u\n",
                     (unsigned long long)punches, (unsigned long long)others,
                     expand_topology ? 1 : 0, primitive_type,
                     expand_topology ? expansion.index_count : element_count, rs.color_mask,
                     rs.blend_control0, (void*)(target ? target->color.Get() : nullptr),
                     cfg.rt_format);
        std::fprintf(f, "PUNCHSHADER ps=%016llX vs=%016llX tex0=0x%08X %ux%u elems=%u | "
                        "shared[slot0]=%u srv_idx=%u slot=%u ntex=%u\n",
                     (unsigned long long)punch_ps, (unsigned long long)punch_vs, punch_tex,
                     punch_texw, punch_texh, punch_elems, punch_shared0, punch_srv, punch_slot,
                     punch_ntex);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  // TEMP DIAG (skip_punch): drop the minimap's circular punch entirely, so its
  // contribution can be isolated by comparing the finished 220x220 target with
  // and without it. Everything about the draw measures correct and the target
  // comes out with no circle, so the first thing to establish is whether it
  // changes ANY pixel.
  // TEMP DIAG (skip_water): pular por familia de shader de agua, para descobrir
  // QUAL delas pinta a faixa clara. 1 = shore, 2 = ocean water, 4 = ocean LOD,
  // 8 = pond. O clarao aparece na zona de arrebentacao (pilares do pier, linha
  // de surf da praia) e nao no mar aberto, o que aponta para o shore -- mas
  // apontar nao e medir.
  {
    const uint32_t skip = uint32_t(REXCVAR_GET(mcla_native_gfx_skip_water));
    if (skip) {
      const bool shore = ps_id == 0x18821F3A51B6E4DEull;
      const bool ocean = ps_id == 0xC5C95CAE57E0D1C4ull;
      const bool oclod = ps_id == 0x3E5818BE5A70E06Bull;
      const bool pond  = ps_id == 0x2EB9178258B7EBDAull;
      // Bits altos: os quatro shaders que desenham DENTRO do passe de reflexo
      // 256x256. O reflexo sai 11x mais claro que a cena do mesmo frame, entao
      // a pergunta passa a ser qual deles o deixa claro.
      const bool seed  = ps_id == 0x35F41762995C91B9ull;  // xrage_postfx__PSSeedRTNoZ
      const bool sky   = ps_id == 0x37BB7CE96694769Eull;  // xSkyhat__ps_main_cheap
      const bool winlod= ps_id == 0xB8444D32CC82F785ull;  // xCityWindowLOD__PSMultiLight
      const bool citlod= ps_id == 0xF6623ADB9AE9F2BEull;  // xCityLOD__PSMultiLight
      if (((skip & 1u) && shore) || ((skip & 2u) && ocean) ||
          ((skip & 4u) && oclod) || ((skip & 8u) && pond) ||
          ((skip & 16u) && seed) || ((skip & 32u) && sky) ||
          ((skip & 64u) && winlod) || ((skip & 128u) && citlod)) {
        return;
      }
    }
  }
  const bool skip_this_punch =
      REXCVAR_GET(mcla_native_gfx_skip_punch) && cfg.width == 220u && cfg.height == 220u &&
      rs.blend_control0 == 0x01810181u && bound_tex_count &&
      bound_tex[0].fetch.base_address == 0x02D64000u;
  // Only the Draw* is skipped, never the dump below: with the switch on the
  // dump is the target BEFORE the punch, with it off the same dump on the same
  // instance is AFTER. Same code path, same counter, so the pair isolates
  // exactly what the punch contributes.
  if (skip_this_punch) {
  } else if (expand_topology) {
    // The generated indices already cover every primitive of the draw, and the
    // counts are small (quads, not streamed geometry), so the >65535 slicer is
    // skipped: its slicing is defined in terms of the ORIGINAL topology and
    // would cut the expanded list at the wrong boundaries.
    cl->DrawIndexedInstanced(expansion.index_count, 1, 0, base_vertex, 0);
    ++g_expanded_draws;
    g_expanded_indices += expansion.index_count;
  } else {
    DrawSlicer slicer(primitive_type, element_count);
    DrawSlice slice;
    while (slicer.Next(slice)) {
      if (bound.indexed) {
        cl->DrawIndexedInstanced(slice.count, 1, start_element + slice.start, base_vertex, 0);
      } else {
        cl->DrawInstanced(slice.count, 1, start_element + slice.start, 0);
      }
    }
  }

  // Hang-find: submit this draw by itself and wait for it with a timeout. The
  // draw whose shader spins forever (the DEVICE_HUNG TDR) is the one that never
  // completes; log its identity before the device is lost.
  if (REXCVAR_GET(mcla_native_gfx_hangfind)) {
    FlushBatch(context);
    if (!context.WaitLastSubmitTimeout(1500)) {
      if (FILE* f = std::fopen("native_gfx_hang.txt", "ab")) {
        std::fprintf(f,
                     "HANG draw#%u vs=%016llX ps=%016llX prim=%u elements=%u is_aux=%d "
                     "target=%ux%u rt_format=%u indexed=%d base_vertex=%d\n",
                     g_cap.accepted, (unsigned long long)vs_id, (unsigned long long)ps_id,
                     primitive_type, element_count, is_aux ? 1 : 0, cfg.width, cfg.height,
                     cfg.rt_format, bound.indexed ? 1 : 0, bound.base_vertex);
        // Vertex streams: a bad stride reads gigabytes; a huge/NaN transform
        // makes degenerate triangles that overdraw the screen for seconds (TDR).
        for (const auto& s : bound.streams) {
          std::fprintf(f, "  stream slot=%u guest_base=0x%08X stride=%u size=%u gpu=0x%llX\n",
                       s.fetch_slot, s.guest_base, s.stride, s.guest_size,
                       (unsigned long long)s.gpu_address);
        }
        // gWorldViewProj (c8..c11) from the VS bank: non-finite here is the
        // classic degenerate-geometry TDR.
        const float* c = reinterpret_cast<const float*>(vs_bank.data() + 8 * 16);
        std::fprintf(f, "  WVP c8..c11:");
        bool nonfinite = false;
        for (int i = 0; i < 16; ++i) {
          std::fprintf(f, " %.4g", c[i]);
          if (!std::isfinite(c[i])) nonfinite = true;
        }
        std::fprintf(f, "   nonfinite=%d\n", nonfinite ? 1 : 0);
        // Bound textures: an unresolved fetch bound with a stale/garbage
        // descriptor can hang the sampler.
        for (uint32_t i = 0; i < bound_tex_count; ++i) {
          const BoundTexture& t = bound_tex[i];
          std::fprintf(f, "  tex slot=%u addr=0x%08X %ux%u fmt=%u resolved=%d srv=%u\n",
                       t.fetch_slot, t.fetch.base_address, t.fetch.width, t.fetch.height,
                       t.fetch.format, t.resolved ? 1 : 0, t.srv_descriptor_index);
        }
        std::fflush(f);
        std::fclose(f);
      }
    }
  }

  CLOGF("%-5u %-6u %016llX %016llX %-8u streams=%zu tex=%u\n", g_cap.accepted, element_count,
        (unsigned long long)vs_id, (unsigned long long)ps_id, primitive_type,
        bound.streams.size(), bound_tex_count);
  // Full texture detail for the first few large draws. A surface rendering
  // flat is either sampling nothing or sampling the wrong thing, and only the
  // per-slot address/format/resolved triple separates those.
  static uint32_t detailed = 0;
  // xCityGrime is the road/asphalt material (PSCityGrime): Diffuse slot 0,
  // Grime slot 1, HeightSpecular slot 2, ShadowCollector slot 13.
  // TEMP INSTRUMENTATION: the full-screen composite/tonemap pass. ps
  // 35F41762995C91B9 is the RECTLIST that covers the LDR target; if it outputs
  // black, either an input texture is black or a tonemap/exposure constant is
  // zero. Dump its slots, its inputs' resolved state, and the first PS ALU
  // constants (where RAGE keeps exposure/tonemap params), once.
  // TEMP INSTRUMENTATION: the luminance/exposure reduction passes (rt=41
  // R32_FLOAT). Logs each one's size, viewport, scissor and inputs so a stage
  // that only writes part of its target (the 4x4 wrote 1 of 16 pixels) is
  // visible.
  if (cfg.rt_format == 41 && g_exposure_lines < 20) {
    ++g_exposure_lines;
    CLOGF("  [expo] %ux%u ps=%016llX prim=%u vp=%.0f,%.0f %.0fx%.0f sc=%ux%u ::",
          cfg.width, cfg.height, (unsigned long long)ps_id, primitive_type, hv.top_left_x,
          hv.top_left_y, hv.width, hv.height, target->key.width, target->key.height);
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      CLOGF(" s%u=0x%08X/%ux%u/f%u/r%d", b.fetch_slot, b.fetch.base_address, b.fetch.width,
            b.fetch.height, b.fetch.format, b.resolved ? 1 : 0);
    }
    CLOGF("\n");
  }

  // Every colour-writing draw of the full-res LDR pass (1280x720 rt=28), with
  // its inputs. The tonemap is the one reading the scene HDR (format 32,
  // 1280x720); this shows whether it runs and whether its inputs resolved.
  // First 40 AND a periodic sample thereafter. A hard cap of 40 covers only the
  // opening frames, so anything that appears later -- the 2D start screen, a
  // menu, a scene change -- was never in the log at all.
  if (cfg.width == 1280 && cfg.height == 720 && cfg.rt_format == 28 && (rs.color_mask & 0xF) &&
      (g_composite_lines < 40 || (g_composite_lines % 2000) == 0)) {
    ++g_composite_lines;
    CLOGF("  [comp] ps=%016llX prim=%u tex=%u blend0=0x%08X colorctl=0x%08X ::",
          (unsigned long long)ps_id, primitive_type, bound_tex_count, rs.blend_control0,
          rs.color_control);
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      CLOGF(" s%u=0x%08X/%ux%u/f%u/r%d", b.fetch_slot, b.fetch.base_address, b.fetch.width,
            b.fetch.height, b.fetch.format, b.resolved ? 1 : 0);
    }
    CLOGF("\n");
    // TEMP DIAG (remove after): route the composite-pass slot dump to the diag
    // file too, since CLOGF is a no-op in continuous mode. Shows whether the
    // tonemap's scene-HDR input resolved (r1) or missed the RT lookup (r0=black).
    if (FILE* fc = std::fopen("native_gfx_diag.txt", "ab")) {
      // vs= added because the composite's UV source has to be read from the
      // vertex shader itself: the declaration says TEXCOORD0 is a single
      // R32_FLOAT, which cannot be right for a fullscreen quad.
      std::fprintf(fc, "[comp] vs=%016llX ps=%016llX prim=%u tex=%u blend0=0x%08X colorctl=0x%08X ::",
                   (unsigned long long)vs_id, (unsigned long long)ps_id, primitive_type,
                   bound_tex_count, rs.blend_control0, rs.color_control);
      for (uint32_t i = 0; i < bound_tex_count; ++i) {
        const BoundTexture& b = bound_tex[i];
        // `src` is the field that matters: R (render-target bridge) is real GPU
        // data, G (guest decode) is only as good as guest memory -- the zero-
        // valued exposure that blacked out this pass logged as a success -- and
        // F is the neutral white substitute.
        std::fprintf(fc, " s%u=0x%08X/%ux%u/f%u/src=%c", b.fetch_slot, b.fetch.base_address,
                     b.fetch.width, b.fetch.height, b.fetch.format,
                     TextureSourceTag(b.source));
      }
      std::fprintf(fc, "\n");
      std::fflush(fc);
      std::fclose(fc);
    }
  }
  if (ps_id == 0x7B5A243101A8F0F9ull && detailed < 4) {
    ++detailed;
    for (uint32_t i = 0; i < bound_tex_count; ++i) {
      const BoundTexture& b = bound_tex[i];
      CLOGF("      slot=%-2u 0x%08X %4ux%-4u format=%-3u tiled=%u resolved=%d srv=%u\n",
            b.fetch_slot, b.fetch.base_address, b.fetch.width, b.fetch.height, b.fetch.format,
            b.fetch.tiled ? 1u : 0u, b.resolved ? 1 : 0, b.srv_descriptor_index);
      // TEMP INSTRUMENTATION: dump the exact resource slot 13 (ShadowCollector)
      // is sampling, once. Tells apart "resolved to a real screen-space shadow
      // buffer" from "decoded raw guest memory that happens to be depth bits".
      if (b.fetch_slot == 13 && b.resource && !g_dumped_collector) {
        g_dumped_collector = true;
        FlushBatch(context);
        RenderTarget tmp;
        tmp.color = b.resource;
        tmp.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        tmp.key.width = b.fetch.width;
        tmp.key.height = b.fetch.height;
        tmp.key.rt_format = 28;  // R8G8B8A8_UNORM, how a k_8_8_8_8 fetch reads
        ReadbackTargetToTga(context, context.device(), tmp,
                            g_cap.dir / "mcla_native_gfx_collector.tga", "slot13 collector");
      }
    }
  }
  // Only the ANCHOR pass counts against the limit. Counting auxiliary draws
  // too meant the shadow and reflection passes ate the budget -- with up to
  // 1200 of them out of 2000, the scene got ~800 draws instead of 2000 and the
  // draw distance visibly collapsed. That was the harness spending the budget,
  // not the renderer dropping geometry.
  if (!is_aux) {
    ++g_cap.accepted;
  }

  if (!(cfg == g_cap.config)) {
    // TEMP DIAG (remove after): the pooled key a composite draw actually
    // RENDERS into, next to the key PrepareContinuousDisplay PRESENTS. ~100
    // composite draws land per frame and the presented target still holds one
    // single colour across all 921600 pixels, which recorded draws cannot
    // explain -- unless they render into a different pooled target. The pool is
    // keyed on ds_format too, so a composite pass using a different depth format
    // gets its own target of the same size and colour format, and what we
    // present is one that was only ever cleared.
    if (is_display) {
      static std::set<uint64_t> seen;
      const RenderTargetKey k = PooledKey(cfg);
      const uint64_t sig = (uint64_t(k.rt_format) << 40) ^ (uint64_t(k.ds_format) << 24) ^
                           (uint64_t(k.width) << 12) ^ uint64_t(k.height);
      if (seen.insert(sig).second) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          // depth_control / color_mask / blend are here because the key match
          // came back 1: the draws DO render into the presented target, so what
          // stops them writing has to be per-draw state. This codebase has hit
          // whole-pass depth rejection before (render_target_pool.h: a reverse-Z
          // clear value left every normal-Z pass rejecting all its geometry).
          const RenderTarget* rt = render_targets.Find(k);
          std::fprintf(f,
                       "COMPKEY draw rt=%u ds=%u %ux%u | readback rt=%u ds=%u %ux%u | match=%d "
                       "| depthctl=0x%08X colormask=0x%08X blend0=0x%08X clear_depth=%.3f\n",
                       k.rt_format, k.ds_format, k.width, k.height, g_cap.readback_key.rt_format,
                       g_cap.readback_key.ds_format, g_cap.readback_key.width,
                       g_cap.readback_key.height,
                       (g_cap.has_readback && k == g_cap.readback_key) ? 1 : 0,
                       rs.depth_control, rs.color_mask, rs.blend_control0,
                       rt ? rt->clear_depth : -1.0f);
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    if (is_display) ++g_cap.accepted_composite; else ++g_cap.accepted_aux;
    // Session peaks. accepted_aux resets every frame, so the true high-water
    // mark is invisible without this -- and with the cap in place the observed
    // maximum is the cap itself, not the demand. The default for
    // mcla_native_gfx_auxcap has to be picked from this number.
    if (g_cap.accepted_aux > g_cap.peak_aux) {
      g_cap.peak_aux = g_cap.accepted_aux;
    }
    if (g_cap.accepted_composite > g_cap.peak_composite) {
      g_cap.peak_composite = g_cap.accepted_composite;
    }
  }
  if (++g_cap.draws_in_batch >= kDrawsPerBatch) {
    if (!FlushBatch(context)) {
      CLOGF("  batch ending at draw %u: EndFrame FAILED\n", g_cap.accepted - 1);
      context.DrainDebugMessages("frame capture batch");
      Finish(context, pipelines, buffers, render_targets, textures.stats(), binder.stats());
      return;
    }
  }

  if (g_cap.accepted >= g_cap.limit && !g_cap.continuous) {
    Finish(context, pipelines, buffers, render_targets, textures.stats(), binder.stats());
  }
}

// Compact "offered vs accepted, and where the rest went" line. The rejection
// counters already existed but only reached the one-shot capture report, so a
// continuous-mode session could run for hours with zero draws recorded and the
// log never said which gate was closing.
std::string DrawRejectionSummary() {
  char buf[448];
  std::snprintf(buf, sizeof(buf),
                "draws offered=%u accepted=%u skipped=%u quadrep=%u/%u(sz=%u rd=%u al=%u ns=%u) expstart=%u | rej: notarmed=%u notidx=%u topo=%u noshader=%u "
                "geom=%u unsup=%u outruns=%u cfg=%u(target=%u budget=%u) packmiss=%u | fail: "
                "bind=%u pso=%u "
                "const=%u",
                g_cap.offered, g_cap.accepted, g_cap.skipped_by_range, g_cap.quad_replicated,
                g_cap.quad_replicate_failed, g_cap.quad_fail_size, g_cap.quad_fail_read,
                g_cap.quad_fail_alloc, g_cap.quad_fail_nostream, g_cap.expanded_nonzero_start,
                g_cap.rej_not_armed,
                g_cap.rej_not_indexed,
                g_cap.rej_topology,
                g_cap.rej_no_shader, g_cap.rej_geometry, g_cap.rej_unsupplied,
                g_cap.rej_outruns_fetch, g_cap.rej_config,
                g_cap.rej_cfg_target, g_cap.rej_cfg_budget, g_cap.rej_shader_missing,
                g_cap.fail_bind, g_cap.fail_pso, g_cap.fail_constants);
  return std::string(buf);
}


void CaptureDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                 uint32_t element_count, uint32_t start_element, int32_t base_vertex, bool indexed,
                 uint32_t draw_limit, D3D12Context& context, ShaderDatabase& shaders,
                 BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                 PipelineCache& pipelines, RenderTargetPool& render_targets, uint32_t aux_stage) {
  CaptureDrawImpl(base, dev, primitive_type, element_count, start_element, base_vertex, indexed,
                  draw_limit, context, shaders, buffers, textures, binder, pipelines,
                  render_targets, aux_stage, nullptr);
}

void SetContinuousMode(bool on) {
  g_cap.continuous = on;
  // Arm immediately so the very first frame is captured. Otherwise the first
  // frame's draws are all dropped by the `!armed` gate (armed is only set at a
  // frame boundary, which for frame 1 has not happened yet), no anchor is ever
  // selected, and no display target is produced.
  if (on) {
    g_cap.armed = true;
  }
}
bool ContinuousMode() { return g_cap.continuous; }

bool PresentContinuousFrame(D3D12Context& context, rex::ui::Presenter* presenter,
                            PresenterOutput& output, BlitPass& blit,
                            RenderTargetPool& render_targets) {
  if (!presenter || !output.initialized() || !blit.initialized()) {
    return false;
  }
  // Close the open draw batch so everything rendered this frame is submitted
  // before it is sampled by the blit.
  FlushBatch(context);

  // The image to show is the last display-shaped colour-writing pass — the
  // composite when it exists, otherwise the scene target. Both are pooled
  // render targets on this same device.
  RenderTarget* display = nullptr;
  if (g_cap.has_readback) {
    display = render_targets.Find(g_cap.readback_key);
  }
  if ((!display || !display->color) && g_cap.has_anchor) {
    display = render_targets.Find(g_cap.anchor_key);
  }
  if (!display || !display->color) {
    return false;
  }
  RenderTarget* src = display;

  struct RecordData {
    RenderTarget* src;
    BlitPass* blit;
  } data{src, &blit};

  return output.PresentFrame(
      context, presenter,
      [](void* user, D3D12Context& ctx, ID3D12GraphicsCommandList* cl, PresenterOutput& out) {
        auto* d = static_cast<RecordData*>(user);
        RenderTarget* s = d->src;
        // The pooled target is left in RENDER_TARGET or COPY_SOURCE state; the
        // blit samples it, so it has to reach PIXEL_SHADER_RESOURCE first.
        const D3D12_RESOURCE_STATES before = s->color_state;
        if (before != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
          D3D12_RESOURCE_BARRIER b = {};
          b.Transition.pResource = s->color.Get();
          b.Transition.StateBefore = before;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
        }
        const float black[4] = {0, 0, 0, 1};
        out.BindAndClear(cl, black);
        // The pooled colour formats (R8G8B8A8 for the composite, the HDR format
        // for the scene) are directly viewable, so the SRV format is the target
        // format itself.
        d->blit->Record(ctx, cl, s->color.Get(), s->key.rt_format);
        // Return it to RENDER_TARGET so the next frame's draws can bind it.
        if (before != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
          D3D12_RESOURCE_BARRIER b = {};
          b.Transition.pResource = s->color.Get();
          b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          b.Transition.StateAfter = before;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
        }
      },
      &data);
}

namespace {
// Published by PrepareContinuousDisplay (guest thread), read by the native
// guest-output callback (command-processor thread). A raw resource pointer plus
// its format/size.
//
// The pointer published here is NOT a pooled render target: it is one of the
// owned display buffers below. Publishing the pooled composite directly was the
// continuous-mode crash. NativeRhiBlitExternalToGuestOutput's contract
// (native_rhi_d3d12.h) requires the source to "stay alive until this submission
// completes", but a pooled target is reused every frame -- the pool rebinds it
// as an RTV next frame (RenderTargetPool::PrepareForRendering) while the command
// processor's blit of THIS frame is still sampling it. That RTV rebind of a
// resource in a shader-read state is an invalid state transition that removes
// the device -> GraphicsSystem::OnHostGpuLossFromAnyThread -> FatalError. So we
// copy the composite into a dedicated, never-rebound texture and publish that.
std::atomic<ID3D12Resource*> g_continuous_display{nullptr};
std::atomic<uint32_t> g_continuous_format{0};
std::atomic<uint32_t> g_continuous_width{0};
std::atomic<uint32_t> g_continuous_height{0};

// Owned display buffers. A small ring so the command processor (which may lag
// the guest thread by a frame) never samples the slot the guest thread is
// currently copying into. Each slot only ever alternates COPY_DEST <->
// ALL_SHADER_RESOURCE and is NEVER bound as an RTV, so it cannot hit the
// pooled-target rebind fault. Slot reuse is fenced by D3D12Context::BeginFrame
// (frames-in-flight wait) since every copy is submitted on the context queue.
struct OwnedDisplayBuffer {
  Microsoft::WRL::ComPtr<ID3D12Resource> tex;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool uav = false;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};
constexpr uint32_t kOwnedDisplayCount = 3;
OwnedDisplayBuffer g_owned_display[kOwnedDisplayCount];
uint32_t g_owned_display_index = 0;
TonemapPass g_tonemap;
GammaPass g_gamma;
FxaaPass g_fxaa;
// FXAA writes here and the ramp/copy below reads it instead of the pooled
// composite. One buffer, not a ring: unlike g_owned_display this is never
// published, so nothing outside this frame's command list ever samples it.
OwnedDisplayBuffer g_fxaa_buffer;
// Fase-A TDR timing: wall-clock start of the current continuous frame's native
// work, set when the previous frame is published/reset.
std::chrono::steady_clock::time_point g_frame_start = std::chrono::steady_clock::now();

// Create (or recreate on size/format/usage change) the slot's texture. Created
// in COPY_DEST because the very first use copies (or dispatches) straight into
// it. `uav` gives it ALLOW_UNORDERED_ACCESS so the tonemap compute pass can
// write it.
bool EnsureOwnedDisplay(ID3D12Device* device, OwnedDisplayBuffer& slot, uint32_t width,
                        uint32_t height, DXGI_FORMAT format, bool uav) {
  if (slot.tex && slot.width == width && slot.height == height && slot.format == format &&
      slot.uav == uav) {
    return true;
  }
  slot.tex.Reset();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(device->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.tex)))) {
    REXLOG_ERROR("[native_gfx] owned display buffer creation failed ({}x{} fmt={})", width, height,
                 uint32_t(format));
    return false;
  }
  slot.uav = uav;
  slot.width = width;
  slot.height = height;
  slot.format = format;
  slot.state = D3D12_RESOURCE_STATE_COPY_DEST;
  return true;
}
}  // namespace

// TEMP DIAG helper: a resolved target's raw bytes, written to a file and
// summarised in the diag. ReadbackTargetToTga interprets the bytes by
// rt_format; the exposure targets are R32_FLOAT and a TGA of them says nothing.
// TEMP DIAG helper: hash and average each 640x640 quadrant of the shadow atlas.
// Two quadrants coming back identical is the cascade that inherited the previous
// cascade depth and rejected every fragment.
static void ReadbackShadowQuadrants(D3D12Context& context, ID3D12Device* device,
                                    RenderTarget& target);

static void ReadbackTargetToRaw(D3D12Context& context, ID3D12Device* device, RenderTarget& target,
                                const char* path) {
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl || !target.color) {
    return;
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = target.color.Get();
  b.Transition.StateBefore = target.color_state;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  const bool move = target.color_state != D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (move) {
    cl->ResourceBarrier(1, &b);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC sd = target.color->GetDesc();
  device->GetCopyableFootprints(&sd, 0, 1, 0, &fp, nullptr, nullptr, &total);
  D3D12_RESOURCE_DESC rb = {};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = total;
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                  D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&readback));
  if (readback) {
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = target.color.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  if (move) {
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
  }
  context.EndFrame();
  context.WaitForIdle();
  if (!readback) {
    return;
  }
  void* mapped = nullptr;
  const D3D12_RANGE range = {0, size_t(total)};
  if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) {
    return;
  }
  if (FILE* raw = std::fopen(path, "wb")) {
    std::fwrite(mapped, 1, size_t(total), raw);
    std::fclose(raw);
  }
  if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
    std::fprintf(f, "EXPOSURE %s %ux%u fmt=%u bytes=%llu rowpitch=%u first:", path,
                 target.key.width, target.key.height, target.key.rt_format,
                 (unsigned long long)total, fp.Footprint.RowPitch);
    const uint32_t n = target.key.width * target.key.height;
    for (uint32_t i = 0; i < n && i < 16u; ++i) {
      float v = 0.0f;
      std::memcpy(&v, static_cast<const uint8_t*>(mapped) + i * 4u, 4);
      std::fprintf(f, " %.6f", v);
    }
    std::fprintf(f, "\n");
    std::fflush(f);
    std::fclose(f);
  }
  readback->Unmap(0, nullptr);
}

static void ReadbackShadowQuadrants(D3D12Context& context, ID3D12Device* device,
                                    RenderTarget& target) {
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl || !target.color) {
    return;
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = target.color.Get();
  b.Transition.StateBefore = target.color_state;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  const bool move = target.color_state != D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (move) {
    cl->ResourceBarrier(1, &b);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC sd = target.color->GetDesc();
  device->GetCopyableFootprints(&sd, 0, 1, 0, &fp, nullptr, nullptr, &total);
  D3D12_RESOURCE_DESC rb = {};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = total;
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                  D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&readback));
  if (readback) {
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = target.color.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  if (move) {
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
  }
  context.EndFrame();
  context.WaitForIdle();
  if (!readback) {
    return;
  }
  void* mapped = nullptr;
  const D3D12_RANGE range = {0, size_t(total)};
  if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) {
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(mapped);
  const uint32_t pitch = fp.Footprint.RowPitch;
  const uint32_t half = target.key.width / 2u;
  uint64_t hash[4] = {};
  double mean[4] = {};
  for (uint32_t q = 0; q < 4u; ++q) {
    const uint32_t x0 = (q & 1u) * half;
    const uint32_t y0 = (q >> 1) * half;
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    double sum = 0.0;
    for (uint32_t y = 0; y < half; ++y) {
      const uint8_t* row = bytes + size_t(y0 + y) * pitch + size_t(x0) * 4u;
      for (uint32_t x = 0; x < half; ++x) {
        uint32_t v = 0;
        std::memcpy(&v, row + x * 4u, 4);
        h = (h ^ v) * 1099511628211ull;
        sum += double(v);
      }
    }
    hash[q] = h;
    mean[q] = sum / double(half * half);
  }
  readback->Unmap(0, nullptr);
  if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
    std::fprintf(f, "SHADOWQUAD hashes %016llX %016llX %016llX %016llX\n",
                 (unsigned long long)hash[0], (unsigned long long)hash[1],
                 (unsigned long long)hash[2], (unsigned long long)hash[3]);
    std::fprintf(f, "  medias %.1f %.1f %.1f %.1f\n", mean[0], mean[1], mean[2], mean[3]);
    bool dup = false;
    for (uint32_t i = 0; i < 4u; ++i) {
      for (uint32_t j = i + 1u; j < 4u; ++j) {
        if (hash[i] == hash[j]) {
          std::fprintf(f, "  IGUAIS: quadrante %u == quadrante %u\n", i, j);
          dup = true;
        }
      }
    }
    if (!dup) {
      std::fprintf(f, "  os quatro sao DISTINTOS\n");
    }
    std::fflush(f);
    std::fclose(f);
  }
}

bool PrepareContinuousDisplay(D3D12Context& context, RenderTargetPool& render_targets) {
  static int pc = 0;
  const bool tr = pc < 4;
  if (tr) REXLOG_INFO("[native_gfx] PrepareDisplay#{} readback={} anchor={} offered={}", pc,
                      g_cap.has_readback ? 1 : 0, g_cap.has_anchor ? 1 : 0, g_cap.offered);
  FlushBatch(context);
  {  // TEMP DIAG (LOWRES): the 320x180 buffer at 0x02DE6000.
    //
    // Its resolve MISSES every single frame (`RESOLVE_MISS dest=0x02DE6000
    // 320x180 count=600`), and the miss detail says why: the key built at
    // resolve time asks for rt_format 28 (R8G8B8A8) while the only 320x180 pass
    // the pool holds is the HDR one (`RESOLVE_DEST dest=0x02D6E000 320x180
    // src_fmt=10`). So nothing native ever writes 0x02DE6000 and any fetch of it
    // decodes GUEST MEMORY -- whatever the title's allocator last left there.
    //
    // That is the shape of the reported artifact: a downsampled post-process
    // buffer read from stale memory is mostly harmless, until a page under it is
    // reused and one texel comes back wildly wrong. At 320x180 one texel is a
    // 4x4 block on a 1280x720 frame, sharp-edged because it is upscaled after
    // the blur -- transient, anywhere, black or bright.
    //
    // This dumps the raw guest bytes so the content can be looked at offline.
    static uint32_t lowres_taken = 0;
    static uint32_t lowres_tick = 0;
    const uint32_t want = REXCVAR_GET(mcla_native_gfx_lowres_dump);
    if (want && lowres_taken < want && (++lowres_tick % 300u) == 0u) {
      // Address and size are cvars so the same probe can be pointed at any
      // surface whose resolve misses. 0x02DE6000 (320x180) came back all zero --
      // that buffer is not the artifact. 0x050A0000 (1024x1024) is the next one:
      // the fast-mipmap chain samples it, and the diag shows the fetch resolving
      // as src=G, a guest-memory decode.
      const uint32_t kAddr = REXCVAR_GET(mcla_native_gfx_lowres_addr);
      const uint32_t kW = REXCVAR_GET(mcla_native_gfx_lowres_w);
      const uint32_t kH = REXCVAR_GET(mcla_native_gfx_lowres_h);
      if (!kAddr || !kW || !kH) {
        lowres_taken = want;
      }
      const uint64_t bytes = uint64_t(kW) * kH * 4ull;
      const uint8_t* phys = TranslatePhysicalGuest(kAddr);
      if (phys && IsPhysicalRangeReadable(kAddr, bytes)) {
        char path[64];
        std::snprintf(path, sizeof(path), "native_gfx_lowres_%u.bin", lowres_taken);
        if (FILE* raw = std::fopen(path, "wb")) {
          std::fwrite(phys, 1, size_t(bytes), raw);
          std::fclose(raw);
        }
        uint32_t lo = 0xFFFFFFFFu, hi = 0, nonzero = 0;
        for (uint32_t t = 0; t < kW * kH; ++t) {
          uint32_t d = 0;
          std::memcpy(&d, phys + t * 4u, 4);
          if (d < lo) lo = d;
          if (d > hi) hi = d;
          if (d) ++nonzero;
        }
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "LOWRES#%u 0x%08X %ux%u min=%08X max=%08X nonzero=%u/%u\n",
                       lowres_taken, kAddr, kW, kH, lo, hi, nonzero, kW * kH);
          std::fflush(f);
          std::fclose(f);
        }
        ++lowres_taken;
      } else if (lowres_taken == 0) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "LOWRES unreadable at 0x%08X\n", kAddr);
          std::fclose(f);
        }
        ++lowres_taken;
      }
    }
  }
  {  // TEMP DIAG (SHADOWQUAD): are the four shadow-atlas quadrants distinct?
    //
    // The shadow pass reuses ONE 640x640 depth surface for the four cascades and
    // resolves each into a quadrant of the 1280x1280 atlas at 0x06E65000. When a
    // cascade inherits the previous cascade depth, every fragment is rejected and
    // its quadrant comes out BIT-IDENTICAL to its neighbour -- that was the
    // "sun only inside a box" bug, and it is back on the GPS map (which is the
    // 3D city at low LOD, so the box is tiny and the whole map reads unlit).
    //
    // This is the measurement that identified it the first time and that the
    // notes list as still not repeated. It reads the RESOLVED copy, never the
    // pooled target -- a pooled 1280x1280 hands back its clear colour through
    // recycling and reads as a false negative.
    static uint32_t sq_taken = 0;
    static uint32_t sq_tick = 0;
    if (REXCVAR_GET(mcla_native_gfx_shadow_quads) &&
        sq_taken < REXCVAR_GET(mcla_native_gfx_shadow_quads) && (++sq_tick % 180u) == 0u) {
      // The atlas is registered by the DEPTH resolve, and its shape follows the
      // time of day (1280x1280 by day, 1280x720 at night were both seen on slot
      // 15), so the lookup has to search rather than assume. Assuming
      // 1280x1280 + want_depth=false found nothing at all.
      D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
      struct Cand { uint32_t w, h; bool depth; };
      static const Cand kCands[] = {
          // COLOUR first, deliberately. For depth the pool registers its OWN
          // surface directly (RegisterDirect) instead of making a copy, and that
          // surface is re-cleared when a full-surface depth resolve ends the
          // pass -- reading it at end of frame gives the cleared far plane
          // (0xFFFFFF in all four quadrants) no matter what the cascades drew.
          // A real resolved COPY, if one exists, is the only trustworthy read.
          {1280, 1280, false}, {1280, 720, false}, {1280, 1280, true}, {1280, 720, true},
      };
      ID3D12Resource* res = nullptr;
      uint32_t hit_w = 0, hit_h = 0;
      bool hit_depth = false;
      for (const Cand& c : kCands) {
        res = render_targets.FindResolvedTarget(0x06E65000u, c.w, c.h, c.depth, &st);
        if (res) { hit_w = c.w; hit_h = c.h; hit_depth = c.depth; break; }
      }
      if (res) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "SHADOWQUAD achou %ux%u depth=%d\n", hit_w, hit_h, hit_depth ? 1 : 0);
          std::fclose(f);
        }
        ++sq_taken;
        RenderTarget tmp;
        tmp.color = res;
        tmp.color_state = st;
        tmp.key.width = hit_w;
        tmp.key.height = hit_h;
        tmp.key.rt_format = 22;
        tmp.key.sample_count = 1;
        ReadbackShadowQuadrants(context, context.device(), tmp);
      } else if (sq_taken == 0) {
        ++sq_taken;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "SHADOWQUAD nenhuma copia resolvida em 0x06E65000 (6 formas tentadas)\n");
          std::fclose(f);
        }
      }
    }
  }
  {  // TEMP DIAG (EXPOSURE): the auto-exposure value the tonemap chain produces.
    //
    // The guest computes an average scene luminance down to a 1x1 target and the
    // post-process chain divides by it. If the native runtime's copy of that
    // number comes out too small, everything downstream is over-exposed --
    // which is what the cam-53 comparison shows: midtones +84 and 11.6% of the
    // frame clipped white against 2.6% on the emulated path, with the SHADOWS
    // matching (p1 19.8 vs 17.8). Darks equal + midtones lifted + highlights
    // clipped is gain before a tonemap, not a gamma curve (the fitted exponent
    // ran 1.1 to 35, so it is not one curve at all).
    //
    // Reads the resolved 1x1 back and prints it as a float, plus the 4x4 stage
    // above it. Both are the addresses the diag already names (FIND / FALLBACK).
    static uint32_t exp_taken = 0;
    static uint32_t exp_tick = 0;
    if (REXCVAR_GET(mcla_native_gfx_exposure_probe) &&
        exp_taken < REXCVAR_GET(mcla_native_gfx_exposure_probe) && (++exp_tick % 200u) == 0u) {
      struct Probe { uint32_t addr, w, h, fmt; const char* name; };
      static const Probe kProbes[] = {
          {0x02D6C000u, 1, 1, 41u, "exposure 1x1"},
          {0x02D6D000u, 4, 4, 41u, "exposure 4x4"},
      };
      ++exp_taken;
      for (const Probe& pr : kProbes) {
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource* res =
            render_targets.FindResolvedTarget(pr.addr, pr.w, pr.h, /*want_depth=*/false, &st);
        if (!res) {
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "EXPOSURE %s 0x%08X SEM ALVO RESOLVIDO\n", pr.name, pr.addr);
            std::fclose(f);
          }
          continue;
        }
        RenderTarget tmp;
        tmp.color = res;
        tmp.color_state = st;
        tmp.key.width = pr.w;
        tmp.key.height = pr.h;
        tmp.key.rt_format = pr.fmt;
        tmp.key.sample_count = 1;
        char path[64];
        std::snprintf(path, sizeof(path), "native_gfx_exposure_%ux%u.bin", pr.w, pr.h);
        ReadbackTargetToRaw(context, context.device(), tmp, path);
      }
    }
  }
  {  // TEMP DIAG (EYECOLLDUMP): the 8x8 shadow collector the eye samples at
    // night, read back from the resolved copy the clear-only path now produces.
    // This is the shader's actual input: the PS scales the eye's light by
    // `saturate(-0.25 + s)`, so white here means lit and zero means black.
    // Done from here, not the draw path, because a readback needs its own
    // command list and a WaitForIdle.
    static bool done = false;
    if (!done && REXCVAR_GET(mcla_native_gfx_eye_probe)) {
      D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
      ID3D12Resource* res =
          render_targets.FindResolvedTarget(0x0329C000u, 8, 8, /*want_depth=*/false, &st);
      if (res) {
        done = true;
        RenderTarget tmp;
        tmp.color = res;
        tmp.color_state = st;
        tmp.key.width = 8;
        tmp.key.height = 8;
        tmp.key.rt_format = 28;
        tmp.key.sample_count = 1;
        ReadbackTargetToTga(context, context.device(), tmp, "native_gfx_eyecoll.tga",
                            "eye shadow collector");
      }
    }
  }
  RenderTarget* display = nullptr;
  // Diagnostic/stopgap: present the anchor (the raw scene target) instead of the
  // readback (composite) pass. In continuous mode the selected readback comes out
  // flat (a constant) while the anchor holds the actual rendered world, so this
  // makes the native scene visible on screen even before the composite selection
  // is fixed.
  const bool present_anchor = REXCVAR_GET(mcla_native_gfx_present_anchor);
  if (present_anchor && g_cap.has_anchor) {
    display = render_targets.Find(g_cap.anchor_key);
  }
  if ((!display || !display->color) && g_cap.has_readback) {
    display = render_targets.Find(g_cap.readback_key);
  }
  if ((!display || !display->color) && g_cap.has_anchor) {
    display = render_targets.Find(g_cap.anchor_key);
  }
  // TEMP DIAG (remove after): why does the frame publish (or not) a display.
  {
    static unsigned total = 0, no_disp = 0, ok = 0;
    ++total;
    const bool published = display && display->color;
    if (published) ++ok; else ++no_disp;
    // Fase-A timing: measured/reset EVERY frame (not only when logged) so the
    // numbers are per-frame. wall = full native frame wall-time; gpu_wait = time
    // the guest thread blocked in BeginFrame's fence wait (proxy for GPU-busy
    // time). gpu_wait≈wall ⇒ GPU-load-bound; gpu_wait<<wall ⇒ CPU-overhead-bound.
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_frame_start)
            .count();
    const double gpu_wait_ms = context.TakeGpuWaitUs() / 1000.0;
    g_frame_start = std::chrono::steady_clock::now();
    if (total <= 10u || (total % 60u) == 0u) {
      static BufferCache::Stats prev_buf;
      static TextureCache::Stats prev_tex;
      static TextureBinder::Stats prev_bind;
      const double* gp = GeometryPhaseMicroseconds();
      static double prev_gp[5] = {};
      auto D = [](uint64_t now, uint64_t before) {
        return (unsigned long long)(now - before);
      };
      if (FILE* fd = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(fd,
                     "prepare total=%u published=%u no_display=%u | anchor=%d readback=%d "
                     "rb=%ux%u/fmt%u | offered=%u acc=%u acc_aux=%u acc_comp=%u | "
                     "rej_cfg=%u (target=%u budget=%u patho=%u) rej_geo=%u rej_shader=%u "
                     "rej_unsup=%u (display=%u) rej_notarmed=%u rej_notidx=%u rej_topo=%u "
                     "rej_nosh=%u | fail_bind=%u fail_pso=%u fail_cb=%u ringflush=%u "
                     "| peak_aux=%u peak_comp=%u srv_exh=%llu smp_exh=%llu "
                     "| wall=%.1fms gpu_wait=%.1fms "
                     "| cpu state=%.1f shader=%.1f geom=%.1f bind=%.1f const=%.1f pso=%.1f ms"
                     "| buf hit=%llu up=%llu reup=%llu unlock=%llu merge=%llu"
                     "| geom up_ms=%.1f up_MB=%.1f reup_MB=%.1f same=%llu"
                     "| inval thunk=%llu/%lluKB unlockKB=%llu dirtied=%llu"
                     "| geomphase ucode=%.1f decl=%.1f match=%.1f idx=%.1f res=%.1f ms"
                     "| invscan steps=%llu regions=%llu"
                     "| tex hit=%llu up=%llu rt=%llu evict=%llu verify=%llu CAUGHT=%llu"
                     "| srv hit=%llu miss=%llu smp hit=%llu miss=%llu unres=%llu"
                     "| memo hit=%llu miss=%llu (guard=%llu key=%llu) bridge=%llu\n",
                     total, ok, no_disp, g_cap.has_anchor ? 1 : 0, g_cap.has_readback ? 1 : 0,
                     display ? display->key.width : 0, display ? display->key.height : 0,
                     display ? display->key.rt_format : 0,
                     g_cap.offered, g_cap.accepted, g_cap.accepted_aux, g_cap.accepted_composite,
                     g_cap.rej_config, g_cap.rej_cfg_target, g_cap.rej_cfg_budget,
                     g_cap.rej_cfg_patho, g_cap.rej_geometry, g_cap.rej_shader_missing,
                     // rej_unsupplied was counted but NEVER surfaced in continuous mode: the
                     // only report that printed it is CLOGF, a no-op there. A draw whose shader
                     // declares an attribute the vertex declaration does not supply (measured:
                     // TEXCOORD1) is dropped outright at frame_capture.cpp:1188.
                     g_cap.rej_unsupplied, g_cap.rej_unsup_display, g_cap.rej_not_armed,
                     g_cap.rej_not_indexed,
                     g_cap.rej_topology,
                     g_cap.rej_no_shader, g_cap.fail_bind, g_cap.fail_pso, g_cap.fail_constants,
                     g_cap.ring_flushes, g_cap.peak_aux, g_cap.peak_composite,
                     (unsigned long long)g_binder_stats_for_report.srv_exhausted,
                     (unsigned long long)g_binder_stats_for_report.sampler_exhausted,
                     wall_ms, gpu_wait_ms, g_profile.state_us / 1000.0,
                     g_profile.shader_us / 1000.0, g_profile.geom_us / 1000.0,
                     g_profile.bind_us / 1000.0, g_profile.const_us / 1000.0,
                     g_profile.pso_us / 1000.0,
                     // Deltas, not totals: a ratio over the whole run hides a cache that
                     // only started thrashing once the city loaded.
                     D(g_buffer_stats_for_report.hits, prev_buf.hits),
                     D(g_buffer_stats_for_report.uploads, prev_buf.uploads),
                     D(g_buffer_stats_for_report.reuploads, prev_buf.reuploads),
                     D(g_buffer_stats_for_report.unlock_invalidations,
                       prev_buf.unlock_invalidations),
                     D(g_buffer_stats_for_report.merges, prev_buf.merges),
                     g_buffer_stats_for_report.upload_us / 1000.0 - prev_buf.upload_us / 1000.0,
                     double(D(g_buffer_stats_for_report.upload_bytes, prev_buf.upload_bytes)) / 1048576.0,
                     double(D(g_buffer_stats_for_report.reupload_bytes, prev_buf.reupload_bytes)) / 1048576.0,
                     D(g_buffer_stats_for_report.reuploads_identical, prev_buf.reuploads_identical),
                     D(g_buffer_stats_for_report.thunk_ranges, prev_buf.thunk_ranges),
                     D(g_buffer_stats_for_report.thunk_bytes, prev_buf.thunk_bytes) / 1024,
                     D(g_buffer_stats_for_report.unlock_bytes, prev_buf.unlock_bytes) / 1024,
                     D(g_buffer_stats_for_report.regions_dirtied, prev_buf.regions_dirtied),
                     gp[0] / 1000.0 - prev_gp[0] / 1000.0, gp[1] / 1000.0 - prev_gp[1] / 1000.0,
                     gp[2] / 1000.0 - prev_gp[2] / 1000.0, gp[3] / 1000.0 - prev_gp[3] / 1000.0,
                     gp[4] / 1000.0 - prev_gp[4] / 1000.0,
                     D(g_buffer_stats_for_report.inval_scan_steps, prev_buf.inval_scan_steps),
                     (unsigned long long)g_buffer_stats_for_report.region_count,
                     D(g_texture_stats_for_report.hits, prev_tex.hits),
                     D(g_texture_stats_for_report.uploads, prev_tex.uploads),
                     D(g_texture_stats_for_report.render_target_hits,
                       prev_tex.render_target_hits),
                     D(g_texture_stats_for_report.evictions, prev_tex.evictions),
                     // The sampled-hash backstop. A non-zero CAUGHT is a lost
                     // invalidation happening right now: the cache was about to
                     // serve bytes the guest had already overwritten. That is the
                     // shape of a transient wrong-looking block in gameplay.
                     D(g_texture_stats_for_report.verify_checks, prev_tex.verify_checks),
                     D(g_texture_stats_for_report.verify_catches, prev_tex.verify_catches),
                     D(g_binder_stats_for_report.srv_hits, prev_bind.srv_hits),
                     D(g_binder_stats_for_report.srv_misses, prev_bind.srv_misses),
                     D(g_binder_stats_for_report.sampler_hits, prev_bind.sampler_hits),
                     D(g_binder_stats_for_report.sampler_misses, prev_bind.sampler_misses),
                     // Slots that resolved to nothing and were pointed at the
                     // neutral white stand-in. Non-zero mid-session is a draw
                     // sampling the WRONG texture for that frame, which is the
                     // shape of a block that appears and vanishes.
                     D(g_binder_stats_for_report.unresolved, prev_bind.unresolved),
                     D(g_binder_stats_for_report.memo_hits, prev_bind.memo_hits),
                     D(g_binder_stats_for_report.memo_misses, prev_bind.memo_misses),
                     D(g_binder_stats_for_report.memo_miss_guard, prev_bind.memo_miss_guard),
                     D(g_binder_stats_for_report.memo_miss_key, prev_bind.memo_miss_key),
                     D(g_binder_stats_for_report.memo_bridge_binds, prev_bind.memo_bridge_binds));
        prev_buf = g_buffer_stats_for_report;
        prev_tex = g_texture_stats_for_report;
        prev_bind = g_binder_stats_for_report;
        for (int gi = 0; gi < 5; ++gi) prev_gp[gi] = gp[gi];
        std::fflush(fd);
        std::fclose(fd);
      }
    }
    g_profile = DrawProfile{};
  }
  if (!display || !display->color) {
    if (tr) { REXLOG_INFO("[native_gfx] PrepareDisplay#{} no display", pc); ++pc; }
    return false;
  }
  if (tr) { REXLOG_INFO("[native_gfx] PrepareDisplay#{} display ok {}x{}", pc, display->key.width,
                        display->key.height); ++pc; }
  // TEMP DIAG (remove after): dump the published display target once (a few
  // frames in, so gameplay is up) to prove whether the native-rendered final
  // image has content or is black.
  {
    static unsigned seen = 0;
    static bool dumped = false;
    ++seen;
    // Any published frame past warmup (this path only runs when a display was
    // selected, so there is always something to inspect).
    // Frame 4 is warmup only: at that point the composite can still be nothing
    // but its clear colour, which reads as "flat" for a reason that has nothing
    // to do with lighting. Re-dump periodically so a LATE frame -- streaming
    // settled, exposure converged -- can be compared against it.
    if ((!dumped && seen >= 4u) || (seen % 900u) == 0u) {
      dumped = true;
      // Readback (the pass we present) and the anchor (main scene) side by side.
      RenderTarget* anchor = g_cap.has_anchor ? render_targets.Find(g_cap.anchor_key) : nullptr;
      if (FILE* fd = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(fd,
                     "DUMP@frame%u readback=%ux%u rt_format=%u samples=%u | anchor=%ux%u "
                     "rt_format=%u samples=%u\n",
                     seen, display->key.width, display->key.height, display->key.rt_format,
                     display->key.sample_count, anchor ? anchor->key.width : 0,
                     anchor ? anchor->key.height : 0, anchor ? anchor->key.rt_format : 0,
                     anchor ? anchor->key.sample_count : 0);
        std::fflush(fd);
        std::fclose(fd);
      }
      // TEMP DIAG (remove after): the minimap target, dumped only on frames that
      // actually drew into it. Corner alpha here answers whether the circular
      // punch erases: if it works the corners are 0, if not they keep the road
      // coverage the content draws left.
      if (g_minimap_seen) {
        if (RenderTarget* mm = render_targets.Find(g_minimap_key)) {
          if (mm->color) {
            char mm_path[64];
            std::snprintf(mm_path, sizeof(mm_path), "native_gfx_minimap_f%u.tga", seen);
            if (FILE* fm = std::fopen("native_gfx_diag.txt", "ab")) {
              std::fprintf(fm, "MINIMAPDUMP f%u rt=%p %ux%u rtfmt=%u\n", seen,
                           (void*)mm->color.Get(), mm->key.width, mm->key.height,
                           mm->key.rt_format);
              std::fflush(fm);
              std::fclose(fm);
            }
            ReadbackTargetToTga(context, context.device(), *mm, mm_path, "minimap 220x220");
          }
        }
        g_minimap_seen = false;
      }
      // TEMP DIAG (REFLDUMP): the water's ReflectionSampler target (slot 3 of
      // xCityOceanWater, a 256x256 resolved surface at 0x05AC3000), dumped raw.
      // The sea is dark from far and blows out white up close while BOTH
      // cameras bind this same address, so the content is the only thing left
      // that can differ. Raw, not TGA: the format is decided by the resource,
      // and interpreting a surface with the wrong one already cost a round.
      {
        static bool refl_dumped = false;
        D3D12_RESOURCE_STATES rst = D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource* refl =
            render_targets.FindResolvedTarget(0x05AC3000u, 256, 256, false, &rst);
        if (!refl_dumped && refl) {
          refl_dumped = true;
          if (ID3D12GraphicsCommandList* rcl = context.BeginFrame()) {
            const D3D12_RESOURCE_DESC rd = refl->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT64 total = 0;
            context.device()->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            D3D12_RESOURCE_DESC rb = {};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = total;
            rb.Height = 1;
            rb.DepthOrArraySize = 1;
            rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Microsoft::WRL::ComPtr<ID3D12Resource> readback;
            context.device()->CreateCommittedResource(
                &rex::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &rb,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
            D3D12_RESOURCE_BARRIER br = {};
            br.Transition.pResource = refl;
            br.Transition.StateBefore = rst;
            br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (rst != D3D12_RESOURCE_STATE_COPY_SOURCE) rcl->ResourceBarrier(1, &br);
            if (readback) {
              D3D12_TEXTURE_COPY_LOCATION d = {}, sl = {};
              d.pResource = readback.Get();
              d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
              d.PlacedFootprint = fp;
              sl.pResource = refl;
              sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              sl.SubresourceIndex = 0;
              rcl->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
            }
            if (rst != D3D12_RESOURCE_STATE_COPY_SOURCE) {
              std::swap(br.Transition.StateBefore, br.Transition.StateAfter);
              rcl->ResourceBarrier(1, &br);
            }
            context.EndFrame();
            context.WaitForIdle();
            void* mapped = nullptr;
            const D3D12_RANGE rng = {0, size_t(total)};
            if (readback && SUCCEEDED(readback->Map(0, &rng, &mapped))) {
              if (FILE* bf = std::fopen("native_gfx_refl.bin", "wb")) {
                std::fwrite(mapped, 1, size_t(total), bf);
                std::fclose(bf);
              }
              readback->Unmap(0, nullptr);
            }
            // E o alvo de CENA no mesmo frame. O reflexo so pode ser chamado de "claro
            // demais" contra alguma referencia, e a referencia certa e o mundo que
            // ele reflete, renderizado com a mesma iluminacao.
            if (anchor && anchor->color) {
              if (ID3D12GraphicsCommandList* scl = context.BeginFrame()) {
                const D3D12_RESOURCE_DESC sd = anchor->color->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT sfp = {};
                UINT64 stot = 0;
                context.device()->GetCopyableFootprints(&sd, 0, 1, 0, &sfp, nullptr, nullptr, &stot);
                D3D12_RESOURCE_DESC srb = {};
                srb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                srb.Width = stot;
                srb.Height = 1;
                srb.DepthOrArraySize = 1;
                srb.MipLevels = 1;
                srb.SampleDesc.Count = 1;
                srb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                Microsoft::WRL::ComPtr<ID3D12Resource> srd;
                context.device()->CreateCommittedResource(
                    &rex::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &srb,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&srd));
                Microsoft::WRL::ComPtr<ID3D12Resource> tmp_ms;
                D3D12_RESOURCE_STATES ast = anchor->color_state;
                ID3D12Resource* asrc = ResolveForReadback(context, scl, *anchor, ast, tmp_ms);
                if (srd && asrc) {
                  D3D12_RESOURCE_BARRIER ab = {};
                  ab.Transition.pResource = asrc;
                  ab.Transition.StateBefore = ast;
                  ab.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                  ab.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                  if (ast != D3D12_RESOURCE_STATE_COPY_SOURCE) scl->ResourceBarrier(1, &ab);
                  D3D12_TEXTURE_COPY_LOCATION dd = {}, ss = {};
                  dd.pResource = srd.Get();
                  dd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                  dd.PlacedFootprint = sfp;
                  ss.pResource = asrc;
                  ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                  ss.SubresourceIndex = 0;
                  scl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
                  if (ast != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                    std::swap(ab.Transition.StateBefore, ab.Transition.StateAfter);
                    scl->ResourceBarrier(1, &ab);
                  }
                }
                context.EndFrame();
                context.WaitForIdle();
                void* sm = nullptr;
                const D3D12_RANGE sr = {0, size_t(stot)};
                if (srd && SUCCEEDED(srd->Map(0, &sr, &sm))) {
                  if (FILE* sf = std::fopen("native_gfx_scene.bin", "wb")) {
                    std::fwrite(sm, 1, size_t(stot), sf);
                    std::fclose(sf);
                  }
                  srd->Unmap(0, nullptr);
                }
                if (FILE* fs = std::fopen("native_gfx_diag.txt", "ab")) {
                  std::fprintf(fs, "SCENEDUMP fmt=%u %llux%u rowpitch=%u total=%llu\n",
                               uint32_t(sd.Format), (unsigned long long)sd.Width, sd.Height,
                               sfp.Footprint.RowPitch, (unsigned long long)stot);
                  std::fflush(fs);
                  std::fclose(fs);
                }
              }
            }
            if (FILE* fr = std::fopen("native_gfx_diag.txt", "ab")) {
              std::fprintf(fr, "REFLDUMP fmt=%u %llux%u rowpitch=%u total=%llu\n",
                           uint32_t(rd.Format), (unsigned long long)rd.Width, rd.Height,
                           fp.Footprint.RowPitch, (unsigned long long)total);
              std::fflush(fr);
              std::fclose(fr);
            }
          }
        }
      }
      char display_path[64], anchor_path[64];
      std::snprintf(display_path, sizeof(display_path), "native_gfx_display_f%u.tga", seen);
      std::snprintf(anchor_path, sizeof(anchor_path), "native_gfx_anchor_f%u.tga", seen);
      ReadbackTargetToTga(context, context.device(), *display, display_path,
                          "continuous display");
      if (anchor && anchor->color) {
        ReadbackTargetToTga(context, context.device(), *anchor, anchor_path, "continuous anchor");
      }
    }
  }
  // Copy the pooled composite into a dedicated display buffer and publish THAT,
  // never the pooled target itself (see the g_owned_display note). The owned
  // slot only alternates COPY_DEST <-> ALL_SHADER_RESOURCE and is never rebound
  // as an RTV, so the command processor can sample it for the whole frame while
  // the pool freely reuses the composite target underneath.
  //
  // ALL_SHADER_RESOURCE (pixel AND non-pixel) is the readable state because the
  // callback's blit is a COMPUTE shader: a compute SRV read demands
  // NON_PIXEL_SHADER_RESOURCE, and PIXEL_SHADER_RESOURCE alone is invalid for it.
  const D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  OwnedDisplayBuffer& slot = g_owned_display[g_owned_display_index];
  g_owned_display_index = (g_owned_display_index + 1) % kOwnedDisplayCount;

  // Present the raw HDR anchor? Then tonemap it (HDR -> LDR R8G8B8A8) instead of
  // a plain copy, so the dark linear scene is viewable. The composite (readback)
  // path is already LDR and just copies. g_tonemap is initialised lazily here.
  const bool want_tonemap = REXCVAR_GET(mcla_native_gfx_present_anchor);
  if (want_tonemap && !g_tonemap.initialized()) {
    g_tonemap.Initialize(context);
  }
  const bool tonemap = want_tonemap && g_tonemap.initialized();
  const DXGI_FORMAT slot_format =
      tonemap ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT(display->key.rt_format);
  // The ramp is a compute pass, so the slot it writes has to carry a UAV even
  // on the plain-copy path.
  const bool want_ramp = !tonemap && REXCVAR_GET(mcla_native_gfx_gamma_ramp);
  if (!EnsureOwnedDisplay(context.device(), slot, display->key.width, display->key.height,
                          slot_format, tonemap || want_ramp)) {
    return false;
  }
  {  // TEMP DIAG (DISPLAY)
    static uint32_t n = 0;
    if (n++ < 12u) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "DISPLAY %ux%u rt=%u samples=%u tonemap=%d\n",
                     display->key.width, display->key.height, display->key.rt_format,
                     display->key.sample_count, tonemap ? 1 : 0);
        std::fclose(f);
      }
    }
  }
  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl) {
    return false;
  }
  if (tonemap) {
    // Source (HDR anchor) -> shader-readable; owned slot -> UAV for the compute
    // write. The tonemap dispatch reads the HDR SRV and writes the LDR UAV.
    D3D12_RESOURCE_BARRIER pre[2] = {};
    uint32_t n = 0;
    if (display->color_state != kReadable) {
      pre[n].Transition.pResource = display->color.Get();
      pre[n].Transition.StateBefore = display->color_state;
      pre[n].Transition.StateAfter = kReadable;
      pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++n;
    }
    if (slot.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      pre[n].Transition.pResource = slot.tex.Get();
      pre[n].Transition.StateBefore = slot.state;
      pre[n].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++n;
    }
    if (n) {
      cl->ResourceBarrier(n, pre);
    }
    g_tonemap.Record(context, cl, display->color.Get(), display->key.rt_format, slot.tex.Get(),
                     display->key.width, display->key.height,
                     float(REXCVAR_GET(mcla_native_gfx_exposure)));
    D3D12_RESOURCE_BARRIER post = {};
    post.Transition.pResource = slot.tex.Get();
    post.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    post.Transition.StateAfter = kReadable;
    post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &post);
    context.EndFrame();
    slot.state = kReadable;
    // Anchor left readable; the pool restores it to RENDER_TARGET on its next
    // PrepareForRendering (which reads color_state).
    display->color_state = kReadable;
  } else {
    // FXAA first, before anything else touches the composite. It writes its own
    // buffer and everything downstream (the gamma ramp, or the plain copy) then
    // reads THAT in place of the pooled target, which is what src_tex/src_state
    // below select. It ends in COPY_SOURCE so the rest of this branch, written
    // for the pooled composite, needs no other change.
    ID3D12Resource* src_tex = display->color.Get();
    uint32_t src_fmt = display->key.rt_format;
    D3D12_RESOURCE_STATES* src_state = &display->color_state;
    bool src_is_fxaa = false;
    // The UAV the shader writes is typed as the slot format, so restrict this
    // to the two formats that view: an unexpected composite format falls
    // through to the untouched copy rather than presenting garbage.
    const bool want_fxaa = REXCVAR_GET(mcla_native_gfx_fxaa) &&
                           display->key.sample_count <= 1 &&
                           (slot_format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                            slot_format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (want_fxaa) {
      if (!g_fxaa.initialized()) {
        g_fxaa.Initialize(context);
      }
      if (g_fxaa.initialized() &&
          EnsureOwnedDisplay(context.device(), g_fxaa_buffer, display->key.width,
                             display->key.height, slot_format, true)) {
        D3D12_RESOURCE_BARRIER pre_fx[2] = {};
        uint32_t fx_count = 0;
        if (display->color_state != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) {
          pre_fx[fx_count].Transition.pResource = display->color.Get();
          pre_fx[fx_count].Transition.StateBefore = display->color_state;
          pre_fx[fx_count].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
          pre_fx[fx_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++fx_count;
        }
        if (g_fxaa_buffer.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
          pre_fx[fx_count].Transition.pResource = g_fxaa_buffer.tex.Get();
          pre_fx[fx_count].Transition.StateBefore = g_fxaa_buffer.state;
          pre_fx[fx_count].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          pre_fx[fx_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++fx_count;
        }
        if (fx_count) {
          cl->ResourceBarrier(fx_count, pre_fx);
        }
        g_fxaa.Record(context, cl, display->color.Get(), display->key.rt_format,
                      g_fxaa_buffer.tex.Get(), uint32_t(slot_format), display->key.width,
                      display->key.height,
                      float(REXCVAR_GET(mcla_native_gfx_fxaa_threshold)),
                      float(REXCVAR_GET(mcla_native_gfx_fxaa_subpixel)));
        D3D12_RESOURCE_BARRIER post_fx = {};
        post_fx.Transition.pResource = g_fxaa_buffer.tex.Get();
        post_fx.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        post_fx.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        post_fx.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &post_fx);
        g_fxaa_buffer.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // The pool restores the composite to RENDER_TARGET from color_state.
        display->color_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        src_tex = g_fxaa_buffer.tex.Get();
        src_fmt = uint32_t(slot_format);
        src_state = &g_fxaa_buffer.state;
        src_is_fxaa = true;
      }
    }

    D3D12_RESOURCE_BARRIER pre[2] = {};
    uint32_t pre_count = 0;
    // Source (composite, or the FXAA buffer) -> COPY_SOURCE.
    if (*src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
      pre[pre_count].Transition.pResource = src_tex;
      pre[pre_count].Transition.StateBefore = *src_state;
      pre[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      pre[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++pre_count;
    }
    // Owned slot -> COPY_DEST.
    if (slot.state != D3D12_RESOURCE_STATE_COPY_DEST) {
      pre[pre_count].Transition.pResource = slot.tex.Get();
      pre[pre_count].Transition.StateBefore = slot.state;
      pre[pre_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      pre[pre_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++pre_count;
    }
    if (pre_count) {
      cl->ResourceBarrier(pre_count, pre);
    }
    // The guest's display gamma ramp goes on here, which is where the
    // emulated path applies it (the command processor's present, as the
    // DC_LUT). Without it every dark tone presents lifted: measured on the
    // night canopy, 58% of the emulated's pixels sit below 0.01 and ours had
    // 0.7%. The ramp is plain guest memory, so this needs no command
    // processor -- see gamma_pass.h. Falls back to the plain copy while the
    // title has not built the table yet, and whenever the cvar is off.
    bool ramped = false;
    if (REXCVAR_GET(mcla_native_gfx_gamma_ramp) && slot.uav) {
      if (!g_gamma.initialized()) {
        g_gamma.Initialize(context);
      }
      const uint8_t* guest_base = rex::Runtime::instance()
                                     ? rex::Runtime::instance()->virtual_membase()
                                     : nullptr;
      if (g_gamma.initialized() && g_gamma.UpdateRamp(context, cl, guest_base)) {
        // The compute pass wants the source readable and the slot writable,
        // which is not the COPY_SOURCE/COPY_DEST pair set up above.
        D3D12_RESOURCE_BARRIER swap[2] = {};
        swap[0].Transition.pResource = src_tex;
        swap[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        swap[0].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        swap[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        swap[1].Transition.pResource = slot.tex.Get();
        swap[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        swap[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        swap[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(2, swap);
        g_gamma.Record(context, cl, src_tex, src_fmt, slot.tex.Get(), display->key.width,
                       display->key.height);
        D3D12_RESOURCE_BARRIER back = {};
        back.Transition.pResource = slot.tex.Get();
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        back.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &back);
        *src_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        ramped = true;
      }
    }
    if (!ramped) {
      cl->CopyResource(slot.tex.Get(), src_tex);
    }
    // Owned slot -> readable for the compute blit.
    D3D12_RESOURCE_BARRIER post = {};
    post.Transition.pResource = slot.tex.Get();
    post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post.Transition.StateAfter = kReadable;
    post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &post);
    context.EndFrame();
    slot.state = kReadable;
    // The pooled composite is left in COPY_SOURCE; the pool restores it to
    // RENDER_TARGET on its next PrepareForRendering, which reads color_state.
    if (!ramped) {
      *src_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    {  // TEMP DIAG (FXAADUMP): the composite and its FXAA output, SAME frame.
      // Same frame matters: comparing two runs, or two frames, mixes the
      // filter's effect with whatever the scene animated in between.
      // Frame gate, learned the hard way: the first frames that publish a
      // display still have nothing streamed in -- black silhouettes, no
      // textures -- so a pair taken there measures a scene that never reaches
      // the player. Wait for a settled frame, then every 900.
      static uint32_t frames = 0;
      static uint32_t taken = 0;
      ++frames;
      const bool settled = frames >= 300u && (frames % 300u) == 0u;
      if (want_fxaa && src_is_fxaa && settled &&
          taken < REXCVAR_GET(mcla_native_gfx_fxaa_dump)) {
        char pre_path[64], post_path[64];
        std::snprintf(pre_path, sizeof(pre_path), "native_gfx_fxaa_pre_%u.tga", taken);
        std::snprintf(post_path, sizeof(post_path), "native_gfx_fxaa_post_%u.tga", taken);
        ++taken;
        RenderTarget fx_tmp;
        fx_tmp.color = g_fxaa_buffer.tex;
        fx_tmp.color_state = g_fxaa_buffer.state;
        fx_tmp.key.width = display->key.width;
        fx_tmp.key.height = display->key.height;
        fx_tmp.key.rt_format = uint32_t(slot_format);
        fx_tmp.key.sample_count = 1;
        ReadbackTargetToTga(context, context.device(), *display, pre_path, "fxaa pre");
        ReadbackTargetToTga(context, context.device(), fx_tmp, post_path, "fxaa post");
        g_fxaa_buffer.state = fx_tmp.color_state;
      }
    }
  }
  // With d3d12_debug on, drain the shared device's debug-layer queue every
  // frame. The continuous-mode crash is a device removal (FatalError 0xC0000409
  // from the presenter, which aborts BEFORE the SDK's DRED dump can run), so the
  // only place the offending invalid API call surfaces is the InfoQueue, and it
  // has to be read the same frame it is recorded, before the async abort. Also
  // dump DRED here if the device is already gone.
  if (REXCVAR_GET(d3d12_debug)) {
    context.DrainDebugMessagesIfAny("continuous frame");
    if (FAILED(context.device()->GetDeviceRemovedReason())) {
      REXLOG_ERROR("[native_gfx] continuous frame: device removed, dumping DRED");
      context.DumpDeviceRemovedData();
    }
  }
  // The published SRV format is the SLOT's format (R8G8B8A8 when tonemapped, the
  // display format when copied), NOT the HDR anchor format.
  g_continuous_format.store(uint32_t(slot_format), std::memory_order_relaxed);
  g_continuous_width.store(display->key.width, std::memory_order_relaxed);
  g_continuous_height.store(display->key.height, std::memory_order_relaxed);
  g_continuous_display.store(slot.tex.Get(), std::memory_order_release);
  return true;
}

void* GetContinuousDisplayResource(uint32_t* srv_format, uint32_t* width, uint32_t* height) {
  ID3D12Resource* r = g_continuous_display.load(std::memory_order_acquire);
  if (srv_format) {
    *srv_format = g_continuous_format.load(std::memory_order_relaxed);
  }
  if (width) {
    *width = g_continuous_width.load(std::memory_order_relaxed);
  }
  if (height) {
    *height = g_continuous_height.load(std::memory_order_relaxed);
  }
  return r;
}

void ResetContinuousFrame(RenderTargetPool& render_targets) {
  // Re-clear every target next frame: continuous mode re-renders the whole scene
  // each frame, so without this the one-shot clear latch leaves moving objects
  // (the helicopter, traffic) trailing their previous positions.
  render_targets.MarkAllUncleared();
  // Keep every cache (targets, resolves, textures, buffers, PSOs). Only the
  // per-frame bookkeeping is cleared so the next frame re-arms and re-selects
  // its anchor/readback without the one-shot latch.
  g_cap.started = false;
  g_cap.armed = true;
  g_cap.end_requested = false;
  g_cap.has_anchor = false;
  g_cap.has_readback = false;
  g_cap.frame_open = false;
  g_cap.draws_in_batch = 0;
  g_cap.offered = 0;
  g_cap.skipped_by_range = 0;
  g_cap.quad_replicated = 0;
  g_cap.quad_replicate_failed = 0;
  g_cap.expanded_nonzero_start = 0;
  g_cap.quad_fail_size = 0;
  g_cap.quad_fail_read = 0;
  g_cap.quad_fail_alloc = 0;
  g_cap.quad_fail_nostream = 0;
  g_cap.accepted = 0;
  g_cap.accepted_aux = 0;
  g_cap.accepted_composite = 0;
  // Safety net: `finished` is a one-shot latch meant for the bounded capture. In
  // continuous mode a single failed batch flush (e.g. a CopyTextureRegion the
  // driver rejects) reaches Finish() and sets it, and without clearing it here
  // CaptureDraw would early-return before `offered++` on EVERY subsequent frame
  // -- the capture dies permanently and the presenter freezes on the last frame.
  // Clearing it each frame lets the capture recover the next frame instead.
  g_cap.finished = false;
}

void CaptureInlineDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                       uint32_t vertex_count, uint32_t stride, uint32_t address,
                       uint32_t draw_limit, D3D12Context& context, ShaderDatabase& shaders,
                       BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                       PipelineCache& pipelines, RenderTargetPool& render_targets,
                       uint32_t aux_stage) {
  ++g_inline_draws;
  g_inline_verts += vertex_count;
  if (primitive_type < 16) {
    ++g_inline_by_prim[primitive_type];
  }
  if (vertex_count == 0 || stride == 0 || address == 0) {
    return;
  }
  InlineGeometry geom;
  geom.address = address;
  // Rounded exactly as the guest packet does: the size field is a dword count,
  // so BeginVertices stores (count * stride) >> 2 dwords back as bytes.
  geom.size_bytes = ((vertex_count * stride) >> 2) * 4u;
  geom.stride = stride;
  CaptureDrawImpl(base, dev, primitive_type, vertex_count, /*start_element=*/0, /*base_vertex=*/0,
                  /*indexed=*/false, draw_limit, context, shaders, buffers, textures, binder,
                  pipelines, render_targets, aux_stage, &geom);
}


// The sample count the pool will use for a target of this shape. The resolve
// path builds its own RenderTargetKey and has to agree with the pool, or its
// Find() misses the target completely: with MSAA on and a hardcoded 1 there,
// the scene resolve missed 1200 times per report, no resolved copy was ever
// produced, and every fetch of the scene fell back to the neutral white
// texture -- a white screen.
uint32_t PooledSampleCountForShape(uint32_t rt_format, uint32_t ds_format, uint32_t width) {
  return PooledSampleCountFields(rt_format, ds_format, width);
}
}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
