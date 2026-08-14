#ifndef REXGLUE_HAS_XEO3_TARGET
// ===========================================================================
// MILESTONE 0 — TEMPORARY INSTRUMENTATION. NOT PART OF THE GAME.
// ===========================================================================
// Passive D3D9-Xenon probe for the Native Graphics Runtime research.
//
// Everything here is gated behind the `mcla_gfx_probe` cvar (default OFF).
// With the cvar off, every hook is a straight pass-through to the original
// recompiled function and the binary behaves exactly as before.
//
// It renders nothing, allocates no GPU resources, patches nothing and changes
// no guest state. It only READS guest memory and appends to an in-process log
// that is flushed to disk at the end of the capture window.
//
// Delete this directory (and its entry in CMakeLists.txt) once Milestone 0 is
// answered.
//
// Addresses used here were confirmed by decompilation:
//   sub_8241D620  D3DDevice_DrawIndexedVertices(dev, primType, baseVertex,
//                                               startIndex, indexCount)
//   sub_8241D230  D3DDevice_DrawVertices(dev, primType, startVertex, count)
//   sub_8241CD88  third draw path (DrawVerticesUP / DrawIndexedVerticesUP)
//   sub_8241BE78  D3DDevice_BeginTiling(dev, flags, nTiles, rects, color, z)
//   sub_8217A470  rage::grcDevice::BeginTiledRendering
//   sub_8217B430  rage::grcDevice::EndTiledRendering  (per-tile loop + EndTiling)
//   sub_8241C308  D3DDevice_EndTiling                 (the resolve)
//   sub_8241BD08  per-tile predication / tile-state select
//   sub_8217B7B0  rage::grcDevice::EndFrame / Present  (frame boundary)
//
// NOT tiling, despite earlier suspicion — kept here so the mistake is not
// repeated: sub_8217AC30 is grcDevice::CreateDevice ("Unable to create D3D
// device", builds the blit shaders and the two vertex declarations) and
// sub_8217AAC0 is grcDevice::CreateVertexDeclaration.
//   sub_82419E98  D3DDevice_Swap
//
// D3DDevice field offsets, confirmed in sub_82424670 / sub_8241D620:
//   +0x030 (48)     command buffer write pointer
//   +0x034 (52)     command buffer hard limit
//   +0x038 (56)     command buffer flush threshold
//   +0x47C (1148)   fetch constant shadow, 32 groups x 6 dwords (768 bytes)
//   +0x780 (1920)   VS ALU float constants, 256 x float4
//   +0x1780 (6016)  PS ALU float constants, 256 x float4
//   +0x2920 (10528) SQ_PROGRAM_CNTL shadow
//   +0x2A80 (10880)
//   +0x2ADC (10940) status byte; bit0 = tiling replay active, bit5 = tiling on
//   +0x2A82 (10370) / +0x2880 (10368) MSAA / surface info
//   +0x3094 (12436) current index buffer object
//   +0x3194 (12692) current pixel shader object
//   +0x3198 (12696) current vertex shader object
//   +0x31CC (12748) tile count set by BeginTiling
//
// Shader object -> microcode, from sub_82424670:
//   PS:  sub = ps + u32(ps + 64);  addr = u32(sub + 40) + u32(ps + 24)
//                                  size = u32(sub + 44)
//   VS:  off = u32(vs + 896 + 8*variant)
//        addr = u32(vs + off + 872) + u32(vs + 32)
//        size = u32(vs + off + 876)
// ===========================================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "mcla_gfx_probe.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(mcla_gfx_probe, false, "MCLA/Debug",
                    "Milestone 0: passive D3D9-Xenon probe. Renders nothing, changes nothing. "
                    "Writes mcla_gfx_probe_*.csv next to the executable.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(mcla_gfx_probe_frames, 900, "MCLA/Debug",
                     "Milestone 0: number of guest frames to capture before the probe dumps its "
                     "report and goes idle (0 = never stop).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gfx_probe {
namespace {

// ---------------------------------------------------------------- guest reads

// Guest -> host translation. NOTE: the 0xE0 physical heap is mapped at a
// +0x1000 host offset on Win32 (rex::memory::detail::PhysicalHostOffset), so
// `base + ea` is NOT valid there. Every read here has to go through GuestPtr.
// Shader microcode lives in that heap, which is exactly what this probe reads.
inline const uint8_t* HostPtr(const uint8_t* base, uint32_t ea) {
  return reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea));
}

// Anything at or above 0x1000 is a candidate: the guest virtual heaps, the XEX
// image and the 0xA0/0xC0/0xE0 physical mirrors all live inside the membase
// reservation. Only the null page is rejected.
inline bool PlausibleEa(uint32_t ea) { return ea >= 0x1000u; }

// Big-endian dword read straight out of the guest image. `base` is the membase
// the recompiled function was handed, so no runtime lookup is needed.
inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (!PlausibleEa(ea)) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, HostPtr(base, ea), 4);
  return __builtin_bswap32(v);
}

inline uint8_t R8(const uint8_t* base, uint32_t ea) {
  if (!PlausibleEa(ea)) {
    return 0;
  }
  return *HostPtr(base, ea);
}

// FNV-1a 64. Deliberately NOT xxHash: the offline side of this experiment is
// regenerated with the same function, so there is no dependency to add here and
// no risk of a version mismatch between the two implementations.
uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  // Hex on purpose: the decimal offset basis previously had a dropped digit
  // (1469598103934665603, one '7' short), making every hash self-consistent
  // but incomparable with the offline table.
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t HashGuestRange(const uint8_t* base, uint32_t ea, uint32_t bytes) {
  // Microcode blobs are small; anything past 1 MB means the pointer chase went
  // off the rails and must not be hashed.
  if (bytes == 0 || bytes > (1u << 20) || !PlausibleEa(ea) ||
      uint64_t(ea) + bytes > 0x100000000ull) {
    return 0;
  }
  return Fnv1a64(HostPtr(base, ea), bytes);
}

// ------------------------------------------------------------ device accessors

constexpr uint32_t kOffFetchShadow = 1148;
constexpr uint32_t kOffVsConst = 1920;
constexpr uint32_t kOffPsConst = 6016;
constexpr uint32_t kOffStatus = 10940;
constexpr uint32_t kOffSurfaceInfo = 10368;
constexpr uint32_t kOffIndexBuffer = 12436;
constexpr uint32_t kOffPixelShader = 12692;
constexpr uint32_t kOffVertexShader = 12696;
constexpr uint32_t kOffTileCount = 12748;

struct UcodeRef {
  uint32_t addr = 0;
  uint32_t size = 0;
  uint64_t hash = 0;
};

UcodeRef PixelShaderUcode(const uint8_t* base, uint32_t ps) {
  UcodeRef r;
  if (!ps) {
    return r;
  }
  const uint32_t sub = ps + R32(base, ps + 64);
  r.addr = R32(base, sub + 40) + R32(base, ps + 24);
  r.size = R32(base, sub + 44);
  r.hash = HashGuestRange(base, r.addr, r.size);
  return r;
}

UcodeRef VertexShaderUcode(const uint8_t* base, uint32_t vs, uint32_t variant) {
  UcodeRef r;
  if (!vs) {
    return r;
  }
  const uint32_t off = R32(base, vs + 896 + 8 * variant);
  if (off == 0) {
    return r;
  }
  r.addr = R32(base, vs + off + 872) + R32(base, vs + 32);
  r.size = R32(base, vs + off + 876);
  r.hash = HashGuestRange(base, r.addr, r.size);
  return r;
}

// ------------------------------------------------------------------ collection

struct DrawKey {
  uint64_t vs_hash;
  uint64_t ps_hash;
  uint32_t prim_type;
  uint32_t indexed;
  uint32_t tiled;
  bool operator==(const DrawKey& o) const {
    return vs_hash == o.vs_hash && ps_hash == o.ps_hash && prim_type == o.prim_type &&
           indexed == o.indexed && tiled == o.tiled;
  }
};

struct DrawKeyHash {
  size_t operator()(const DrawKey& k) const {
    uint64_t h = k.vs_hash * 1099511628211ull ^ k.ps_hash;
    h = h * 1099511628211ull ^ (uint64_t(k.prim_type) << 2 | k.indexed << 1 | k.tiled);
    return size_t(h);
  }
};

struct DrawAgg {
  uint64_t count = 0;
  uint64_t total_elements = 0;
  uint32_t min_elements = 0xFFFFFFFFu;
  uint32_t max_elements = 0;
  uint64_t sliced = 0;
  uint32_t vs_size = 0;
  uint32_t ps_size = 0;
};

struct ShaderAgg {
  uint64_t draws = 0;
  uint32_t size = 0;
  uint32_t is_pixel = 0;
  uint32_t first_frame = 0;
  // First 16 microcode bytes. If the runtime hash fails to match the offline
  // table, this says whether the blob differs (guest-side fixup) or whether the
  // two hashes were simply computed over different ranges.
  uint8_t head[16] = {};
  // Full microcode as the GPU sees it. head16 proved the start address is right
  // and the size agrees, yet the hashes differ -- so the bytes must diverge
  // somewhere past offset 16. Only the real blob can say where, and guessing
  // costs another capture. Dumped to mcla_gfx_probe_ucode/<hash>.bin.
  std::vector<uint8_t> blob;
};

struct TileBracket {
  uint32_t frame = 0;
  uint32_t begin_seq = 0;   // draw sequence when the bracket opened
  uint32_t end_seq = 0;     // draw sequence when it closed
  uint32_t tiles = 0;
  uint32_t caller = 0;      // grcDevice::BeginTiledRendering call site marker
  uint32_t surface_info = 0;      // r6 of BeginTiledRendering (format selector),
                                  // NOT the RB_SURFACE_INFO register
  uint32_t endtiling_flags = 0;   // D3DDevice_EndTiling args, 0 if never called
  uint32_t endtiling_rects = 0;
  uint32_t endtiling_dest = 0;
  uint32_t endtiling_seen = 0;
};

std::mutex g_mutex;
std::atomic<bool> g_active{false};
std::atomic<bool> g_done{false};

uint32_t g_frame = 0;
uint64_t g_draw_seq = 0;
uint64_t g_draws_this_frame = 0;
uint64_t g_max_draws_frame = 0;
uint64_t g_draws_indexed = 0;
uint64_t g_draws_nonindexed = 0;
uint64_t g_draws_up = 0;
uint64_t g_draws_while_tiled = 0;

// Raw dump of the first draws, so a hash that fails to match the offline table
// can be debugged without another run: it shows the object pointers and the
// microcode address/size the accessors derived from them.
struct RawDraw {
  uint32_t frame, dev, vs_obj, ps_obj, vs_addr, vs_size, ps_addr, ps_size;
  uint32_t prim, elements, indexed, status;
  uint64_t vs_hash, ps_hash;
};
constexpr size_t kRawDrawMax = 128;
std::vector<RawDraw> g_raw;

std::unordered_map<DrawKey, DrawAgg, DrawKeyHash> g_draws;
std::unordered_map<uint64_t, ShaderAgg> g_shaders;
std::unordered_map<uint32_t, uint64_t> g_prim_hist;
std::vector<TileBracket> g_brackets;
TileBracket g_open_bracket;
bool g_bracket_open = false;
uint64_t g_msaa_seen = 0;
uint32_t g_last_surface_info = 0;

}  // namespace

// External linkage from here on: these are the capture entry points called
// by the graphics hooks in native_gfx/hooks.cpp (declared in the header).

bool Enabled() { return REXCVAR_GET(mcla_gfx_probe) && !g_done.load(std::memory_order_relaxed); }

void RecordShader(const uint8_t* base, const UcodeRef& u, uint32_t is_pixel) {
  if (!u.hash) {
    return;
  }
  auto& s = g_shaders[u.hash];
  if (s.draws == 0) {
    s.size = u.size;
    s.is_pixel = is_pixel;
    s.first_frame = g_frame;
    const uint32_t n = u.size < 16u ? u.size : 16u;
    std::memcpy(s.head, HostPtr(base, u.addr), n);
    if (u.size <= (64u << 10)) {
      s.blob.assign(HostPtr(base, u.addr), HostPtr(base, u.addr) + u.size);
    }
  }
  ++s.draws;
}

void RecordDraw(const uint8_t* base, uint32_t dev, uint32_t prim_type, uint32_t elements,
                uint32_t indexed, bool is_up) {
  const uint32_t status = R8(base, dev + kOffStatus);
  const uint32_t tiled = (status & 1u) ? 1u : 0u;

  const uint32_t vs = R32(base, dev + kOffVertexShader);
  const uint32_t ps = R32(base, dev + kOffPixelShader);
  const UcodeRef v = VertexShaderUcode(base, vs, 0);
  const UcodeRef p = PixelShaderUcode(base, ps);

  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_draw_seq;
  ++g_draws_this_frame;
  if (indexed) {
    ++g_draws_indexed;
  } else {
    ++g_draws_nonindexed;
  }
  if (is_up) {
    ++g_draws_up;
  }
  if (tiled) {
    ++g_draws_while_tiled;
  }
  ++g_prim_hist[prim_type & 0x3F];

  RecordShader(base, v, 0);
  RecordShader(base, p, 1);

  DrawKey k{v.hash, p.hash, prim_type & 0x3F, indexed, tiled};
  auto& a = g_draws[k];
  ++a.count;
  a.total_elements += elements;
  a.min_elements = elements < a.min_elements ? elements : a.min_elements;
  a.max_elements = elements > a.max_elements ? elements : a.max_elements;
  if (elements > 0xFFFFu) {
    ++a.sliced;
  }
  a.vs_size = v.size;
  a.ps_size = p.size;

  // Sample across the whole session instead of burning the budget on frame 0,
  // which is all bootstrap blits.
  if (g_raw.size() < kRawDrawMax && (g_draw_seq % 4096) == 1) {
    g_raw.push_back(RawDraw{g_frame, dev, vs, ps, v.addr, v.size, p.addr, p.size,
                            prim_type, elements, indexed, status, v.hash, p.hash});
  }

  g_last_surface_info = R32(base, dev + kOffSurfaceInfo);
}

// ---------------------------------------------------------------------- report

std::filesystem::path ReportPath(const char* suffix) {
  std::error_code ec;
  auto dir = std::filesystem::current_path(ec);
  if (ec) {
    dir = std::filesystem::path(".");
  }
  return dir / (std::string("mcla_gfx_probe_") + suffix + ".csv");
}

void WriteReport() {
  std::lock_guard<std::mutex> lock(g_mutex);

  // --- XEX-embedded shader containers (Milestone 0 follow-up, condition 1).
  // 119 ShaderContainers live in the XEX data section (RAGE/D3D internal
  // blit/clear/resolve/UI shaders, confirmed via IDA). The on-disk XEX is
  // LZX-compressed, so the pristine image bytes are dumped from guest memory
  // here instead. Range covers first container 0x827D2E1E .. last 0x82823920
  // (+0x134 body), rounded out.
  {
    constexpr uint32_t kXexShaderLo = 0x827D0000u;
    constexpr uint32_t kXexShaderHi = 0x82824000u;
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    if (ec) {
      dir = std::filesystem::path(".");
    }
    FILE* f = std::fopen((dir / "mcla_gfx_probe_xex_shaders.bin").string().c_str(), "wb");
    if (f) {
      auto* runtime = rex::Runtime::instance();
      const uint8_t* base = runtime->memory()->virtual_membase();
      std::fwrite(HostPtr(base, kXexShaderLo), 1, kXexShaderHi - kXexShaderLo, f);
      std::fclose(f);
    }
  }

  // --- raw microcode, one file per unique blob, for a byte-exact offline diff
  {
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    if (ec) {
      dir = std::filesystem::path(".");
    }
    dir /= "mcla_gfx_probe_ucode";
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
      for (const auto& [h, s] : g_shaders) {
        if (s.blob.empty()) {
          continue;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "%016llX_%s.bin", (unsigned long long)h,
                      s.is_pixel ? "ps" : "vs");
        FILE* f = std::fopen((dir / name).string().c_str(), "wb");
        if (f) {
          std::fwrite(s.blob.data(), 1, s.blob.size(), f);
          std::fclose(f);
        }
      }
    }
  }

  // --- summary
  {
    auto p = ReportPath("summary");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f, "key,value\n");
      std::fprintf(f, "frames,%u\n", g_frame);
      std::fprintf(f, "draws_total,%llu\n", (unsigned long long)g_draw_seq);
      std::fprintf(f, "draws_indexed,%llu\n", (unsigned long long)g_draws_indexed);
      std::fprintf(f, "draws_nonindexed,%llu\n", (unsigned long long)g_draws_nonindexed);
      std::fprintf(f, "draws_up_path,%llu\n", (unsigned long long)g_draws_up);
      std::fprintf(f, "draws_while_tiling_active,%llu\n", (unsigned long long)g_draws_while_tiled);
      std::fprintf(f, "max_draws_in_one_frame,%llu\n", (unsigned long long)g_max_draws_frame);
      std::fprintf(f, "unique_shader_ucode,%zu\n", g_shaders.size());
      std::fprintf(f, "unique_draw_states,%zu\n", g_draws.size());
      std::fprintf(f, "tiling_brackets,%zu\n", g_brackets.size());
      std::fprintf(f, "last_surface_info,0x%08X\n", g_last_surface_info);
      // RB_SURFACE_INFO.msaa_samples is at bits 16..17. An earlier version of
      // this line masked bits 0..1 (part of surface_pitch), read 0, and was
      // reported as "MSAA is 1x, confirmed" — it was not. The game programs
      // 2x and 4x, cross-confirmed by PA_SU_SC_MODE_CNTL.msaa_enable.
      std::fprintf(f, "msaa_samples_field,%u\n", (g_last_surface_info >> 16) & 3u);
      std::fclose(f);
    }
  }

  // --- shaders actually used (the T1 coverage answer)
  {
    auto p = ReportPath("shaders");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f, "ucode_fnv1a64,stage,ucode_bytes,draws,first_frame,head16\n");
      for (const auto& [h, s] : g_shaders) {
        char head[33];
        const uint32_t n = s.size < 16u ? s.size : 16u;
        for (uint32_t i = 0; i < n; ++i) {
          std::snprintf(head + 2 * i, 3, "%02X", s.head[i]);
        }
        head[2 * n] = '\0';
        std::fprintf(f, "%016llX,%s,%u,%llu,%u,%s\n", (unsigned long long)h,
                     s.is_pixel ? "ps" : "vs", s.size, (unsigned long long)s.draws,
                     s.first_frame, head);
      }
      std::fclose(f);
    }
  }

  // --- per draw-state profile
  {
    auto p = ReportPath("draws");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f,
                   "vs_fnv1a64,ps_fnv1a64,prim_type,indexed,tiled,count,avg_elements,"
                   "min_elements,max_elements,sliced\n");
      for (const auto& [k, a] : g_draws) {
        std::fprintf(f, "%016llX,%016llX,%u,%u,%u,%llu,%llu,%u,%u,%llu\n",
                     (unsigned long long)k.vs_hash, (unsigned long long)k.ps_hash, k.prim_type,
                     k.indexed, k.tiled, (unsigned long long)a.count,
                     (unsigned long long)(a.count ? a.total_elements / a.count : 0),
                     a.min_elements == 0xFFFFFFFFu ? 0 : a.min_elements, a.max_elements,
                     (unsigned long long)a.sliced);
      }
      std::fclose(f);
    }
  }

  // --- primitive topology histogram
  {
    auto p = ReportPath("prims");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f, "prim_type,draws\n");
      for (const auto& [t, c] : g_prim_hist) {
        std::fprintf(f, "%u,%llu\n", t, (unsigned long long)c);
      }
      std::fclose(f);
    }
  }

  // --- raw first-draws dump (debug aid for a hash mismatch)
  {
    auto p = ReportPath("raw");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f,
                   "frame,dev,vs_obj,ps_obj,vs_ucode_addr,vs_ucode_bytes,ps_ucode_addr,"
                   "ps_ucode_bytes,prim,elements,indexed,status,vs_fnv1a64,ps_fnv1a64\n");
      for (const auto& r : g_raw) {
        std::fprintf(f,
                     "%u,0x%08X,0x%08X,0x%08X,0x%08X,%u,0x%08X,%u,%u,%u,%u,0x%02X,%016llX,"
                     "%016llX\n",
                     r.frame, r.dev, r.vs_obj, r.ps_obj, r.vs_addr, r.vs_size, r.ps_addr,
                     r.ps_size, r.prim, r.elements, r.indexed, r.status,
                     (unsigned long long)r.vs_hash, (unsigned long long)r.ps_hash);
      }
      std::fclose(f);
    }
  }

  // --- tiling brackets
  {
    auto p = ReportPath("tiling");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
      std::fprintf(f,
                   "frame,tiles,draws_inside,caller_marker,begin_fmt_sel,endtiling_seen,"
                   "endtiling_flags,endtiling_rects,endtiling_dest\n");
      for (const auto& b : g_brackets) {
        std::fprintf(f, "%u,%u,%u,%u,0x%08X,%u,0x%08X,0x%08X,0x%08X\n", b.frame, b.tiles,
                     b.end_seq >= b.begin_seq ? b.end_seq - b.begin_seq : 0, b.caller,
                     b.surface_info, b.endtiling_seen, b.endtiling_flags, b.endtiling_rects,
                     b.endtiling_dest);
      }
      std::fclose(f);
    }
  }

  REXLOG_INFO("[mcla_gfx_probe] report written: {} frames, {} draws, {} unique ucode, {} brackets",
              g_frame, (unsigned long long)g_draw_seq, g_shaders.size(), g_brackets.size());
}

void OnFrameEnd() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame;
  if (g_draws_this_frame > g_max_draws_frame) {
    g_max_draws_frame = g_draws_this_frame;
  }
  g_draws_this_frame = 0;
  const int32_t limit = REXCVAR_GET(mcla_gfx_probe_frames);
  if (limit > 0 && g_frame >= uint32_t(limit) && !g_done.load(std::memory_order_relaxed)) {
    g_done.store(true, std::memory_order_relaxed);
  }
}

void Install() {
  if (!REXCVAR_GET(mcla_gfx_probe)) {
    return;
  }
  g_active.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[mcla_gfx_probe] active — passive capture for {} frames",
              REXCVAR_GET(mcla_gfx_probe_frames));
}

void OpenBracket(uint32_t caller_marker, uint32_t surface_info) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_open_bracket = TileBracket{};
  g_open_bracket.frame = g_frame;
  g_open_bracket.begin_seq = uint32_t(g_draw_seq);
  g_open_bracket.caller = caller_marker;
  g_open_bracket.surface_info = surface_info;
  g_bracket_open = true;
}

void SetBracketTiles(uint32_t tiles) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_bracket_open) {
    g_open_bracket.tiles = tiles;
  }
}

void NoteEndTiling(uint32_t flags, uint32_t rects, uint32_t dest) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_bracket_open) {
    return;
  }
  g_open_bracket.endtiling_flags = flags;
  g_open_bracket.endtiling_rects = rects;
  g_open_bracket.endtiling_dest = dest;
  g_open_bracket.endtiling_seen = 1;
}

void CloseBracket() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_bracket_open) {
    return;
  }
  g_open_bracket.end_seq = uint32_t(g_draw_seq);
  g_brackets.push_back(g_open_bracket);
  g_bracket_open = false;
}

}  // namespace mcla::gfx_probe

// NOTE: the REX_FUNC hook bodies used to live here. They moved to
// src/native_gfx/hooks.cpp, the single owner of the guest graphics hooks
// (a rex_sub_* weak-symbol override can only be defined once per binary,
// and the Native Graphics Runtime needs the same interception points).
// The probe behavior is unchanged: hooks call back into this file's
// RecordDraw / bracket / report entry points, gated by the same cvar.

#endif // REXGLUE_HAS_XEO3_TARGET
