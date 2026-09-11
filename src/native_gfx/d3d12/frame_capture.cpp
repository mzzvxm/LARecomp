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
REXCVAR_DECLARE(bool, mcla_native_gfx_half_pixel);
REXCVAR_DECLARE(bool, mcla_native_gfx_swapped_texcoords);

namespace mcla::native_gfx {

namespace {

constexpr float kClearColor[4] = {0.02f, 0.02f, 0.04f, 1.0f};
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
  uint32_t ds_format = 0;
  uint32_t sample_count = 1;
  uint32_t width = 0;
  uint32_t height = 0;
  bool operator==(const TargetConfig& o) const {
    return rt_format == o.rt_format && ds_format == o.ds_format &&
           sample_count == o.sample_count && width == o.width && height == o.height;
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

// Pooled targets are SINGLE-SAMPLED regardless of what the guest asks for.
// A multisampled depth surface cannot be viewed as a Texture2D, and D3D12 has
// neither a depth resolve nor a depth-to-colour copy, so an MSAA shadow map
// could never be sampled. The guest's own resolve always lands a
// single-sampled image in main memory — MSAA only ever exists inside EDRAM —
// so this simplifies the intermediate, not the result.
RenderTargetKey PooledKey(const TargetConfig& cfg) {
  RenderTargetKey k;
  k.rt_format = cfg.rt_format;
  k.ds_format = cfg.ds_format;
  k.sample_count = 1;
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
  ID3D12Resource* src = anchor->color.Get();
  // Pooled targets are single-sampled, so this is a straight copy.
  // The state has to come from the target, not be assumed: the composite pass
  // is left in COPY_SOURCE by its own resolve, and declaring RENDER_TARGET here
  // would be a lie to the runtime.
  const D3D12_RESOURCE_STATES entry_state = anchor->color_state;
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
  ID3D12Resource* src = target.color.Get();
  const D3D12_RESOURCE_STATES entry_state = target.color_state;
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
      g_cap.readback_key = PooledKey(cfg);
      g_cap.readback_config = cfg;
      g_cap.has_readback = true;
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
  const uint64_t vs_id = ShaderIdentity(
      reinterpret_cast<const uint8_t*>(
          rex::memory::GuestPtr(const_cast<uint8_t*>(base), vsr.guest_address)),
      vsr.size_bytes);
  const uint64_t ps_id =
      depth_only ? 0
                 : ShaderIdentity(reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
                                      const_cast<uint8_t*>(base), psr.guest_address)),
                                  psr.size_bytes);
  const uint32_t ps_spec = rs.alpha_test_enable ? 2u : 0u;
  const ShaderBytecode vs_code = shaders.Lookup(vs_id, 0, /*is_pixel=*/false);
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
  if (buffers.stats().upload_failures != buffer_upload_failures_before) {
    ++g_cap.ring_flushes;
    FlushBatch(context);
    ++g_cap.fail_bind;
    return;
  }
  if (!bound.complete) {
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
  binder.BindAll(context, cl, base, dev, textures, shared.data(), bound_tex, &bound_tex_count, 32);
  ProfileAdd(g_profile.bind_us, t_bind);
  g_binder_stats_for_report = binder.stats();
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
  // so shifting the sample point by half a pixel is -1/W in x and +1/H in y,
  // measured against the VIEWPORT, not the target.
  if (REXCVAR_GET(mcla_native_gfx_half_pixel) && hv.width > 0.0f && hv.height > 0.0f) {
    shared_values.half_pixel_offset[0] = -1.0f / hv.width;
    shared_values.half_pixel_offset[1] = 1.0f / hv.height;
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
  PsoKey key = PipelineCache::MakeKey(bound, rs, vs_id, ps_id, 0, ps_spec);
  // Pooled targets are single-sampled; a PSO whose sample count disagrees with
  // the bound target is rejected outright.
  key.sample_count = 1;
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
  RenderTarget* target = render_targets.Acquire(context, PooledKey(cfg), clear_depth);
  // TEMP INSTRUMENTATION: accumulated per pass, not sampled from one draw.
  // Sampling the first draw was misleading: it is a depth prepass with the
  // colour mask at zero, which looks identical to a pass that never writes.
  NotePassDraw(cfg.width, cfg.height, cfg.rt_format, rs.color_mask);
  if (!target) {
    CLOGF("      no render target: %ux%u rt=%u ds=%u samples=%u\n", cfg.width, cfg.height,
          cfg.rt_format, cfg.ds_format, cfg.sample_count);
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
  render_targets.PrepareForRendering(cl, *target);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = target->rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = target->dsv_heap->GetCPUDescriptorHandleForHeapStart();
  cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  if (!target->cleared) {
    // Cleared once per target: every later draw of that pass accumulates into
    // it, which is what makes the shadow atlas and the scene build up.
    target->cleared = true;
    cl->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              target->clear_depth, 0, 0, nullptr);
    // TEMP SKYPROBE: whichever draw first touches the anchor decides its depth
    // clear. If that draw is not reverse-Z the anchor depth clears to 1.0 and
    // the reverse-Z sky (z=0, GEQUAL, depth_wr=0) is rejected everywhere.
    if (cfg == g_cap.config) {
      static uint32_t n = 0;
      if (n++ < 8) {
        REXLOG_INFO("[SKYCLEAR] anchor first-draw cleardepth={:.1f} vpz={:.3f}..{:.3f} "
                    "vs={:016X} ps={:016X} func={} depth_wr={}",
                    target->clear_depth, hv.min_depth, hv.max_depth, vs_id, ps_id, rs.depth_func,
                    rs.depth_write ? 1 : 0);
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
  cl->IASetVertexBuffers(0, UINT(vbvs.size()), vbvs.data());
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
  if (expand_topology) {
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

bool PrepareContinuousDisplay(D3D12Context& context, RenderTargetPool& render_targets) {
  static int pc = 0;
  const bool tr = pc < 4;
  if (tr) REXLOG_INFO("[native_gfx] PrepareDisplay#{} readback={} anchor={} offered={}", pc,
                      g_cap.has_readback ? 1 : 0, g_cap.has_anchor ? 1 : 0, g_cap.offered);
  FlushBatch(context);
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
      if (FILE* fd = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(fd,
                     "prepare total=%u published=%u no_display=%u | anchor=%d readback=%d "
                     "rb=%ux%u/fmt%u | offered=%u acc=%u acc_aux=%u acc_comp=%u | "
                     "rej_cfg=%u (target=%u budget=%u patho=%u) rej_geo=%u rej_shader=%u "
                     "rej_unsup=%u (display=%u) rej_notidx=%u rej_topo=%u "
                     "rej_nosh=%u | fail_bind=%u fail_pso=%u fail_cb=%u ringflush=%u "
                     "| peak_aux=%u peak_comp=%u srv_exh=%llu smp_exh=%llu "
                     "| wall=%.1fms gpu_wait=%.1fms "
                     "| cpu state=%.1f shader=%.1f geom=%.1f bind=%.1f const=%.1f pso=%.1f ms\n",
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
                     g_cap.rej_unsupplied, g_cap.rej_unsup_display, g_cap.rej_not_indexed,
                     g_cap.rej_topology,
                     g_cap.rej_no_shader, g_cap.fail_bind, g_cap.fail_pso, g_cap.fail_constants,
                     g_cap.ring_flushes, g_cap.peak_aux, g_cap.peak_composite,
                     (unsigned long long)g_binder_stats_for_report.srv_exhausted,
                     (unsigned long long)g_binder_stats_for_report.sampler_exhausted,
                     wall_ms, gpu_wait_ms, g_profile.state_us / 1000.0,
                     g_profile.shader_us / 1000.0, g_profile.geom_us / 1000.0,
                     g_profile.bind_us / 1000.0, g_profile.const_us / 1000.0,
                     g_profile.pso_us / 1000.0);
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
                     "DUMP@frame%u readback=%ux%u rt_format=%u | anchor=%ux%u rt_format=%u\n",
                     seen, display->key.width, display->key.height, display->key.rt_format,
                     anchor ? anchor->key.width : 0, anchor ? anchor->key.height : 0,
                     anchor ? anchor->key.rt_format : 0);
        std::fflush(fd);
        std::fclose(fd);
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
  if (!EnsureOwnedDisplay(context.device(), slot, display->key.width, display->key.height,
                          slot_format, tonemap)) {
    return false;
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
    D3D12_RESOURCE_BARRIER pre[2] = {};
    uint32_t pre_count = 0;
    // Source: pooled composite -> COPY_SOURCE.
    if (display->color_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
      pre[pre_count].Transition.pResource = display->color.Get();
      pre[pre_count].Transition.StateBefore = display->color_state;
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
    cl->CopyResource(slot.tex.Get(), display->color.Get());
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
    display->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
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

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
