#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — resource telemetry.
// See telemetry.h. Deliberately self-contained and mutex-guarded; the draw
// hooks call in from the guest render thread.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "telemetry.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "d3d12/pipeline_cache.h"
#include "d3d12/shader_db.h"
#include "d3d12/texture_binding.h"
#include "draw_slicing.h"
#include "geometry.h"
#include "guest/guest_constants.h"
#include "guest/render_state.h"
#include "guest/sampler_state.h"
#include "guest/guest_resources.h"
#include "guest/texture_format.h"
#include "shader_identity.h"

REXCVAR_DEFINE_BOOL(mcla_native_gfx_geomdump, false, "MCLA/NativeGfx",
                    "Dump a per-draw geometry snapshot (streams, strides, input layout, index "
                    "buffer, slicing) built from guest state, to "
                    "mcla_native_gfx_geometry.txt. One entry per distinct draw shape.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_texdump, false, "MCLA/NativeGfx",
                    "Decode the first textures the game binds straight out of guest memory "
                    "(fetch constant -> format -> untile) and write them to "
                    "mcla_native_gfx_textures/ for offline inspection. This is the correctness "
                    "test for the texture decoder against real game data; off by default "
                    "because it writes tens of MB.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(mcla_native_gfx_telemetry, true, "MCLA/NativeGfx",
                    "Collect resource-usage telemetry (bound buffers, index buffers, texture "
                    "fetch constants) while the native graphics runtime is active. Written to "
                    "mcla_native_gfx_resources_*.csv. Cheap; on by default during bring-up.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::native_gfx {

namespace {

inline const uint8_t* HostPtr(const uint8_t* base, uint32_t ea) {
  return reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea));
}

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, HostPtr(base, ea), 4);
  return __builtin_bswap32(v);
}

struct VbStats {
  uint64_t draws = 0;
  uint32_t endian = 0;
  uint32_t first_frame = 0;
};

struct IbStats {
  uint64_t draws = 0;
  uint32_t bits = 16;
  uint32_t endian_raw = 0;
};

struct TexKey {
  uint32_t d[6];
  bool operator<(const TexKey& o) const {
    for (int i = 0; i < 6; ++i) {
      if (d[i] != o.d[i]) return d[i] < o.d[i];
    }
    return false;
  }
};

struct ShaderStats {
  uint64_t draws = 0;
  uint32_t size = 0;
  bool is_pixel = false;
  bool resolved = false;  // found in the shader pack
  uint32_t dxil_size = 0;
};

std::mutex g_mutex;
// key = (addr << 32) | size — a range, not just an address, so overlapping
// and subrange bindings show up as distinct entries for offline analysis.
std::map<uint64_t, VbStats> g_vbufs;
std::map<uint64_t, IbStats> g_ibufs;
std::map<TexKey, uint64_t> g_texs;
uint64_t g_draws = 0;
uint32_t g_frame = 0;
// One-shot raw dump of the fetch-constant shadow, captured mid-frame deep
// into the session. Decides empirically whether the shadow groups start at
// +1148 or carry a header (the flush decompilation is ambiguous about a
// +4 skew, and the first telemetry run saw zero texture fetches — a
// misalignment would explain exactly that).
bool g_shadow_dumped = false;
uint8_t g_shadow_raw[1024];
uint32_t g_shadow_dev = 0;

// Live validation of the shader database: every draw resolves its bound
// VS/PS microcode to a pack entry. This is the end-to-end proof that the
// runtime identity (vfetch-normalized, from the patched microcode the GPU
// actually executes) matches the offline pack keys.
ShaderDatabase g_shader_db;
bool g_shader_db_tried = false;
std::map<uint64_t, ShaderStats> g_shaders;
uint64_t g_draws_vs_resolved = 0;
uint64_t g_draws_ps_resolved = 0;
uint64_t g_draws_no_ps_bound = 0;
uint64_t g_draws_pack_miss = 0;

// A shader slot at a draw. kNotBound (no shader object, e.g. depth-only
// passes) is a legitimate state and must NOT be conflated with kPackMiss,
// which is a real coverage failure.
enum class ShaderResolve { kResolved, kNotBound, kPackMiss };

// --- texture decode validation ---------------------------------------------
// Decodes a texture the game just bound, straight out of guest memory, using
// exactly the production path (DecodeTextureFetch -> GetFormatInfo ->
// UntileSurface2D). Writes the linear result plus its metadata so it can be
// turned into a viewable image offline (tools/dump_textures_to_png.py).
// This is how the texture decoder is proven against real game data before
// any of it reaches a draw.
uint32_t g_tex_dumped = 0;
constexpr uint32_t kMaxTexDumps = 64;
constexpr uint32_t kMaxTexDumpBytes = 8u << 20;
FILE* g_tex_index = nullptr;
// Why textures were skipped. Without this the dump silently produces nothing
// and there is no way to tell a decode gate from a memory-validity gate.
uint64_t g_tex_skip_invalid = 0;
uint64_t g_tex_skip_rt_format = 0;
uint64_t g_tex_skip_unsupported = 0;
uint64_t g_tex_skip_size = 0;
uint64_t g_tex_skip_unreadable = 0;
uint64_t g_tex_skip_io = 0;

// Geometry diagnostic state.
uint32_t g_geom_dumped = 0;
constexpr uint32_t kMaxGeomDumps = 64;
FILE* g_geom_file = nullptr;
std::set<uint64_t> g_geom_shapes;
// PSO key validation: the same guest state must always produce the same key,
// and any relevant change a different one. Keys are hashed here without
// creating pipelines, so this runs with no D3D12 device involved.
std::map<size_t, uint32_t> g_pso_keys;   // hash -> first draw that produced it
uint64_t g_pso_key_hits = 0;

// Full PSO cache telemetry, independent of the filtered dump above. The dump
// records one draw per distinct SHAPE, so by construction it never sees two
// identical draws and can never exercise the HIT path. This counter builds
// the key for EVERY draw (up to a budget, since the snapshot walk is not
// free) and measures the real hit/miss ratio.
constexpr uint64_t kPsoCounterBudget = 20000;
std::map<size_t, uint64_t> g_pso_counter_keys;  // hash -> times seen
uint64_t g_pso_counter_draws = 0;
uint64_t g_pso_counter_hits = 0;
uint64_t g_pso_counter_misses = 0;
uint64_t g_pso_counter_skipped = 0;

void DumpTexture(const uint8_t* base, const uint32_t d[6]) {
  if (!REXCVAR_GET(mcla_native_gfx_texdump) || g_tex_dumped >= kMaxTexDumps) {
    return;
  }
  const TextureFetch f = DecodeTextureFetch(d);
  if (!f.type_valid || f.width == 0 || f.height == 0 || f.base_address < 0x1000u) {
    ++g_tex_skip_invalid;
    return;
  }
  // Render-target-sourced formats do not live in guest memory.
  if (IsRenderTargetSourcedFormat(f.format)) {
    ++g_tex_skip_rt_format;
    return;
  }
  const FormatInfo fi = GetFormatInfo(f.format);
  if (fi.bytes_per_block == 0) {
    ++g_tex_skip_unsupported;
    return;
  }

  const uint32_t wb = (f.width + fi.block_width - 1) / fi.block_width;
  const uint32_t hb = (f.height + fi.block_height - 1) / fi.block_height;
  const uint64_t linear_size = uint64_t(wb) * hb * fi.bytes_per_block;
  if (linear_size == 0 || linear_size > kMaxTexDumpBytes) {
    ++g_tex_skip_size;
    return;
  }
  // The whole source extent must be committed guest memory. Being inside the
  // 4 GiB reservation is not enough — the reservation is sparse, and a stale
  // fetch constant pointing at an uncommitted page faults the process.
  const uint64_t src_size =
      f.tiled ? TiledSurfaceSizeBytes(wb, hb, fi.bytes_per_block)
              : uint64_t(hb) * (f.pitch ? (f.pitch / fi.block_width) * fi.bytes_per_block
                                        : wb * fi.bytes_per_block);
  if (!IsPhysicalRangeReadable(f.base_address, src_size)) {
    ++g_tex_skip_unreadable;
    if (g_tex_skip_unreadable <= 5) {
      REXLOG_ERROR("[native_gfx] texdump: range not readable {:#010x}+{} ({}x{} fmt {} tiled {})",
                   f.base_address, src_size, f.width, f.height, f.format, f.tiled ? 1 : 0);
    }
    return;
  }

  std::error_code ec;
  auto dir = std::filesystem::current_path(ec);
  if (ec) {
    dir = std::filesystem::path(".");
  }
  dir /= "mcla_native_gfx_textures";
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    ++g_tex_skip_io;
    return;
  }

  std::vector<uint8_t> linear;
  linear.resize(size_t(linear_size));
  // Texture fetch constants carry PHYSICAL addresses (the Xenos reads memory
  // directly); they must go through the physical membase.
  const uint8_t* src = TranslatePhysicalGuest(f.base_address);
  if (!src) {
    ++g_tex_skip_unreadable;
    return;
  }
  const uint32_t pitch = wb * fi.bytes_per_block;
  if (f.tiled) {
    // Tiled surfaces are padded to 32x32-block macro tiles and are larger
    // than wb*hb; the untiler needs the real readable extent.
    UntileSurface2D(linear.data(), pitch, src,
                    TiledSurfaceSizeBytes(wb, hb, fi.bytes_per_block), wb, hb,
                    fi.bytes_per_block);
  } else {
    const uint32_t guest_pitch =
        f.pitch ? (f.pitch / fi.block_width) * fi.bytes_per_block : pitch;
    for (uint32_t y = 0; y < hb; ++y) {
      std::memcpy(linear.data() + size_t(y) * pitch, src + size_t(y) * guest_pitch, pitch);
    }
  }

  char name[80];
  std::snprintf(name, sizeof(name), "tex%03u_%08X_f%02u_%ux%u.bin", g_tex_dumped, f.base_address,
                f.format, f.width, f.height);
  if (FILE* out = std::fopen((dir / name).string().c_str(), "wb")) {
    std::fwrite(linear.data(), 1, linear.size(), out);
    std::fclose(out);
  }
  if (!g_tex_index) {
    g_tex_index = std::fopen((dir / "index.csv").string().c_str(), "wb");
    if (g_tex_index) {
      std::fprintf(g_tex_index,
                   "file,base_address,format,width,height,tiled,pitch,endian,swizzle,"
                   "block_w,block_h,bytes_per_block\n");
    }
  }
  if (g_tex_index) {
    std::fprintf(g_tex_index, "%s,0x%08X,%u,%u,%u,%u,%u,%u,0x%03X,%u,%u,%u\n", name,
                 f.base_address, f.format, f.width, f.height, f.tiled ? 1u : 0u, f.pitch,
                 f.endianness, f.swizzle, fi.block_width, fi.block_height, fi.bytes_per_block);
    std::fflush(g_tex_index);
  }
  ++g_tex_dumped;
}

ShaderResolve ResolveShader(const uint8_t* base, const ShaderUcodeRef& ref, bool is_pixel) {
  if (!ref.valid() || ref.size_bytes > kMaxUcodeBytes ||
      !IsGuestRangeReadable(ref.guest_address, ref.size_bytes)) {
    return ShaderResolve::kNotBound;
  }
  const uint8_t* ucode = reinterpret_cast<const uint8_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), ref.guest_address));
  const uint64_t key = ShaderIdentity(ucode, ref.size_bytes);
  if (key == 0) {
    return ShaderResolve::kNotBound;
  }
  auto& s = g_shaders[key];
  if (s.draws == 0) {
    s.size = ref.size_bytes;
    s.is_pixel = is_pixel;
    // Variant 0 is enough for the lookup proof; the spec-constant variant is
    // selected by the draw pipeline once render state feeds it.
    const ShaderBytecode bc = g_shader_db.Lookup(key, 0, is_pixel);
    s.resolved = bc.valid();
    s.dxil_size = bc.size;
  }
  ++s.draws;
  return s.resolved ? ShaderResolve::kResolved : ShaderResolve::kPackMiss;
}

void WriteReports() {
  std::error_code ec;
  auto dir = std::filesystem::current_path(ec);
  if (ec) {
    dir = std::filesystem::path(".");
  }
  auto open = [&](const char* suffix) {
    return std::fopen(
        (dir / (std::string("mcla_native_gfx_resources_") + suffix + ".csv")).string().c_str(),
        "wb");
  };

  if (FILE* f = open("vbuf")) {
    std::fprintf(f, "guest_addr,size,endian,draws,first_frame\n");
    for (const auto& [k, s] : g_vbufs) {
      std::fprintf(f, "0x%08X,%u,%u,%llu,%u\n", uint32_t(k >> 32), uint32_t(k), s.endian,
                   (unsigned long long)s.draws, s.first_frame);
    }
    std::fclose(f);
  }
  if (FILE* f = open("ibuf")) {
    std::fprintf(f, "guest_addr,bits,endian_raw,draws\n");
    for (const auto& [k, s] : g_ibufs) {
      std::fprintf(f, "0x%08X,%u,0x%08X,%llu\n", uint32_t(k >> 32), s.bits, s.endian_raw,
                   (unsigned long long)s.draws);
    }
    std::fclose(f);
  }
  if (FILE* f = open("tex")) {
    std::fprintf(f, "d0,d1,d2,d3,d4,d5,draws\n");
    for (const auto& [k, n] : g_texs) {
      std::fprintf(f, "%08X,%08X,%08X,%08X,%08X,%08X,%llu\n", k.d[0], k.d[1], k.d[2], k.d[3],
                   k.d[4], k.d[5], (unsigned long long)n);
    }
    std::fclose(f);
  }
  if (FILE* f = open("shaders")) {
    std::fprintf(f, "identity,stage,ucode_bytes,draws,resolved,dxil_bytes\n");
    for (const auto& [k, s] : g_shaders) {
      std::fprintf(f, "%016llX,%s,%u,%llu,%d,%u\n", (unsigned long long)k,
                   s.is_pixel ? "ps" : "vs", s.size, (unsigned long long)s.draws,
                   s.resolved ? 1 : 0, s.dxil_size);
    }
    std::fclose(f);
  }
  if (g_shadow_dumped) {
    if (FILE* f = open("shadowraw")) {
      std::fprintf(f, "dev,0x%08X\nbase_offset,%u\n", g_shadow_dev, kDevFetchShadowOffset - 64);
      for (uint32_t i = 0; i < sizeof(g_shadow_raw); i += 4) {
        uint32_t v;
        std::memcpy(&v, g_shadow_raw + i, 4);
        std::fprintf(f, "%u,%08X\n", i, __builtin_bswap32(v));
      }
      std::fclose(f);
    }
  }
  if (FILE* f = open("summary")) {
    // Overlap statistics: how many distinct ranges share a start address and
    // how many ranges intersect a differently-keyed range. Decides whether
    // guest_addr alone can key the buffer cache.
    uint32_t same_start = 0, intersecting = 0;
    uint32_t prev_addr = 0, prev_end = 0;
    bool first = true;
    for (const auto& [k, s] : g_vbufs) {
      const uint32_t addr = uint32_t(k >> 32), size = uint32_t(k);
      if (!first) {
        if (addr == prev_addr) ++same_start;
        if (addr < prev_end) ++intersecting;
      }
      prev_addr = addr;
      prev_end = addr + size > prev_end ? addr + size : prev_end;
      first = false;
    }
    std::fprintf(f, "key,value\n");
    std::fprintf(f, "frames,%u\n", g_frame);
    std::fprintf(f, "draws_observed,%llu\n", (unsigned long long)g_draws);
    std::fprintf(f, "unique_vbuf_ranges,%zu\n", g_vbufs.size());
    std::fprintf(f, "vbuf_same_start_diff_size,%u\n", same_start);
    std::fprintf(f, "vbuf_intersecting_ranges,%u\n", intersecting);
    std::fprintf(f, "unique_ibufs,%zu\n", g_ibufs.size());
    std::fprintf(f, "unique_texture_fetches,%zu\n", g_texs.size());
    // Shader database validation.
    size_t resolved = 0;
    for (const auto& [k, s] : g_shaders) {
      resolved += s.resolved ? 1 : 0;
    }
    std::fprintf(f, "shader_pack_loaded,%d\n", g_shader_db.loaded() ? 1 : 0);
    std::fprintf(f, "shader_pack_entries,%u\n", g_shader_db.entry_count());
    std::fprintf(f, "unique_shaders_bound,%zu\n", g_shaders.size());
    std::fprintf(f, "unique_shaders_resolved,%zu\n", resolved);
    std::fprintf(f, "draws_vs_resolved,%llu\n", (unsigned long long)g_draws_vs_resolved);
    std::fprintf(f, "draws_ps_resolved,%llu\n", (unsigned long long)g_draws_ps_resolved);
    std::fprintf(f, "texdump_written,%u\n", g_tex_dumped);
    std::fprintf(f, "texdump_skip_invalid,%llu\n", (unsigned long long)g_tex_skip_invalid);
    std::fprintf(f, "texdump_skip_rt_format,%llu\n", (unsigned long long)g_tex_skip_rt_format);
    std::fprintf(f, "texdump_skip_unsupported,%llu\n",
                 (unsigned long long)g_tex_skip_unsupported);
    std::fprintf(f, "texdump_skip_size,%llu\n", (unsigned long long)g_tex_skip_size);
    std::fprintf(f, "texdump_skip_unreadable,%llu\n", (unsigned long long)g_tex_skip_unreadable);
    std::fprintf(f, "texdump_skip_io,%llu\n", (unsigned long long)g_tex_skip_io);
    // PSO cache, measured over every draw rather than the filtered dump.
    uint64_t repeated = 0, max_seen = 0;
    for (const auto& [h, n] : g_pso_counter_keys) {
      if (n > 1) {
        ++repeated;
      }
      if (n > max_seen) {
        max_seen = n;
      }
    }
    std::fprintf(f, "pso_draws_analyzed,%llu\n", (unsigned long long)g_pso_counter_draws);
    std::fprintf(f, "pso_unique_keys,%zu\n", g_pso_counter_keys.size());
    std::fprintf(f, "pso_cache_hits,%llu\n", (unsigned long long)g_pso_counter_hits);
    std::fprintf(f, "pso_cache_misses,%llu\n", (unsigned long long)g_pso_counter_misses);
    std::fprintf(f, "pso_hit_rate_pct,%.2f\n",
                 g_pso_counter_draws
                     ? 100.0 * double(g_pso_counter_hits) / double(g_pso_counter_draws)
                     : 0.0);
    std::fprintf(f, "pso_repeated_keys,%llu\n", (unsigned long long)repeated);
    std::fprintf(f, "pso_max_uses_of_one_key,%llu\n", (unsigned long long)max_seen);
    std::fprintf(f, "pso_draws_skipped_incomplete,%llu\n",
                 (unsigned long long)g_pso_counter_skipped);
    std::fprintf(f, "draws_no_ps_bound,%llu\n", (unsigned long long)g_draws_no_ps_bound);
    // The only number that means a coverage failure.
    std::fprintf(f, "draws_shader_pack_miss,%llu\n", (unsigned long long)g_draws_pack_miss);
    std::fclose(f);
  }
}

}  // namespace

void TelemetryRecordDraw(const uint8_t* base, uint32_t dev, uint32_t prim_type, uint32_t elements,
                         bool indexed) {
  if (!REXCVAR_GET(mcla_native_gfx_telemetry)) {
    return;
  }
  (void)prim_type;
  (void)elements;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_draws;

  // Raw shadow snapshot: once, deep into the session, mid-frame — kept as a
  // permanent sanity check of the +1152 group alignment (it originally
  // diagnosed the 4-byte header). Starts 64 bytes before the shadow base.
  if (!g_shadow_dumped && g_frame >= 300 && (g_draws % 512) == 100) {
    std::memcpy(g_shadow_raw, HostPtr(base, dev + kDevFetchShadowOffset - 64),
                sizeof(g_shadow_raw));
    g_shadow_dev = dev;
    g_shadow_dumped = true;
  }

  // Walk the fetch constant shadow: 32 groups x 6 dwords. A group whose
  // first dword has type bits == 2 is a texture fetch; otherwise each of its
  // three dword pairs may be a vertex fetch (type bits == 3).
  const uint32_t shadow = dev + kDevFetchShadowOffset;
  for (uint32_t g = 0; g < kFetchShadowGroups; ++g) {
    const uint32_t ea = shadow + g * kFetchGroupDwords * 4;
    const uint32_t d0 = R32(base, ea);
    if ((d0 & 3u) == 2u) {
      TexKey key;
      for (int i = 0; i < 6; ++i) {
        key.d[i] = R32(base, ea + 4 * i);
      }
      const bool first_time = g_texs.find(key) == g_texs.end();
      ++g_texs[key];
      if (first_time) {
        DumpTexture(base, key.d);
      }
      continue;
    }
    for (uint32_t p = 0; p < 3; ++p) {
      const VertexFetch f =
          DecodeVertexFetch(R32(base, ea + 8 * p), R32(base, ea + 8 * p + 4));
      if (!f.valid()) {
        continue;
      }
      auto& s = g_vbufs[(uint64_t(f.guest_address) << 32) | f.size_bytes];
      if (s.draws == 0) {
        s.endian = f.endian;
        s.first_frame = g_frame;
      }
      ++s.draws;
    }
  }

  // Shader resolution (VS + PS bound at this draw).
  if (!g_shader_db_tried) {
    g_shader_db_tried = true;
    g_shader_db.Load();
  }
  if (g_shader_db.loaded()) {
    const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
    const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
    // Variant selection matters: depth-only passes use variant 1 when the
    // shader has one. Always reading variant 0 resolved a DIFFERENT shader
    // for those draws without ever reporting a miss (variant 0 is in the
    // pack too), which is exactly the kind of silent error to avoid.
    const ShaderUcodeRef vs = ReadVertexShaderUcode(
        base, vs_obj, SelectVertexShaderVariant(base, vs_obj, ps_obj));
    const ShaderUcodeRef ps = ReadPixelShaderUcode(base, ps_obj);
    const ShaderResolve vs_r = ResolveShader(base, vs, /*is_pixel=*/false);
    const ShaderResolve ps_r = ResolveShader(base, ps, /*is_pixel=*/true);
    g_draws_vs_resolved += vs_r == ShaderResolve::kResolved ? 1 : 0;
    g_draws_ps_resolved += ps_r == ShaderResolve::kResolved ? 1 : 0;
    // Depth-only / shadow passes legitimately bind no pixel shader.
    g_draws_no_ps_bound += ps_r == ShaderResolve::kNotBound ? 1 : 0;
    if (vs_r == ShaderResolve::kPackMiss || ps_r == ShaderResolve::kPackMiss) {
      ++g_draws_pack_miss;
    }
  }

  if (indexed) {
    const uint32_t ib_obj = R32(base, dev + kDevIndexBufferOffset);
    if (ib_obj) {
      const GuestIndexBuffer ib = DecodeIndexBuffer(
          R32(base, ib_obj), R32(base, ib_obj + kIndexBufferObjAddrOffset));
      auto& s = g_ibufs[uint64_t(ib.guest_address) << 32];
      if (s.draws == 0) {
        s.bits = ib.indices_32bit ? 32 : 16;
        s.endian_raw = ib.endian;
      }
      ++s.draws;
    }
  }
}

void TelemetryRecordGeometry(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                             uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                             bool indexed, uint64_t vs_dirty, uint64_t ps_dirty) {
  if (!REXCVAR_GET(mcla_native_gfx_geomdump)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_geom_dumped >= kMaxGeomDumps) {
    return;
  }
  if (!g_shader_db.loaded()) {
    return;
  }
  // Guest-only snapshot: no BufferCache/command list, so this captures what
  // the native runtime WOULD bind without touching D3D12.
  const GeometrySnapshot s =
      BuildGeometrySnapshot(base, dev, primitive_type, element_count, start_element, base_vertex,
                            indexed, g_shader_db, nullptr, nullptr, nullptr);

  // Full cache telemetry first: this must see EVERY draw, unlike the dump
  // below which keeps one draw per distinct shape and therefore can never
  // exercise the HIT path.
  if (!s.complete) {
    ++g_pso_counter_skipped;
  } else if (g_pso_counter_draws < kPsoCounterBudget) {
    ++g_pso_counter_draws;
    const GuestRenderState rs_all = ReadRenderState(base, dev);
    const uint32_t vso = R32(base, dev + kDevVertexShaderOffset);
    const uint32_t pso_obj = R32(base, dev + kDevPixelShaderOffset);
    uint64_t vsid = 0, psid = 0;
    const ShaderUcodeRef vr =
        ReadVertexShaderUcode(base, vso, SelectVertexShaderVariant(base, vso, pso_obj));
    if (vr.valid() && vr.size_bytes <= kMaxUcodeBytes &&
        IsGuestRangeReadable(vr.guest_address, vr.size_bytes)) {
      vsid = ShaderIdentity(HostPtr(base, vr.guest_address), vr.size_bytes);
    }
    const ShaderUcodeRef pr = ReadPixelShaderUcode(base, pso_obj);
    if (pr.valid() && pr.size_bytes <= kMaxUcodeBytes &&
        IsGuestRangeReadable(pr.guest_address, pr.size_bytes)) {
      psid = ShaderIdentity(HostPtr(base, pr.guest_address), pr.size_bytes);
    }
    const PsoKey k_all =
        PipelineCache::MakeKey(s, rs_all, vsid, psid, 0, rs_all.alpha_test_enable ? 2u : 0u);
    auto& seen = g_pso_counter_keys[PsoKeyHash{}(k_all)];
    if (seen == 0) {
      ++g_pso_counter_misses;
    } else {
      ++g_pso_counter_hits;
    }
    ++seen;
  }

  // Only dump interesting, distinct draws: one per (prim, stream count,
  // input layout size) shape, so the file stays readable.
  const uint64_t shape = (uint64_t(primitive_type) << 32) ^ (s.streams.size() << 16) ^
                         s.input_layout.size() ^ (uint64_t(s.complete) << 60);
  if (!g_geom_shapes.insert(shape).second) {
    return;
  }

  std::error_code ec;
  auto dir = std::filesystem::current_path(ec);
  if (ec) {
    dir = std::filesystem::path(".");
  }
  if (!g_geom_file) {
    g_geom_file = std::fopen((dir / "mcla_native_gfx_geometry.txt").string().c_str(), "wb");
    if (!g_geom_file) {
      return;
    }
  }
  FILE* f = g_geom_file;
  std::fprintf(f, "=== draw %u  frame %u ===\n", g_geom_dumped, g_frame);

  // Raw shader-object state. The vfetch patcher (sub_82423A38, driven by
  // sub_82424670/sub_82424530) is skipped entirely when the device has no
  // vertex declaration bound (dev+11820) or when bit 0x40 of the shader
  // descriptor at obj+872 is set — so an unpatched shader has three possible
  // causes and only this dump tells them apart.
  {
    const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
    const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
    const uint32_t decl = R32(base, dev + 11820);
    const uint32_t flags = vs_obj ? R32(base, vs_obj + 872) : 0;
    const uint32_t chosen = SelectVertexShaderVariant(base, vs_obj, ps_obj);
    std::fprintf(f, "  vs_obj=0x%08X ps_obj=0x%08X decl=0x%08X flags872=0x%08X variant=%u\n",
                 vs_obj, ps_obj, decl, flags, chosen);

    // Declaration + stride table: the authoritative layout source. Printed
    // raw so the snapshot can be checked against it, including the
    // stream -> fetch slot mapping (95 - stream).
    if (decl) {
      const uint32_t n = R32(base, decl + kDeclOffsetElementCount);
      std::fprintf(f, "    decl elements=%u max_stream=%u\n", n,
                   R32(base, decl + kDeclOffsetMaxStream));
      if (n <= 64 && IsGuestRangeReadable(decl + kDeclOffsetElements, uint64_t(n) * 12)) {
        const auto* recs = reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
            const_cast<uint8_t*>(base), decl + kDeclOffsetElements));
        for (uint32_t i = 0; i < n; ++i) {
          const DeclarationElement de = ReadDeclarationElement(recs, i);
          const uint32_t stride =
              uint32_t(*reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
                  const_cast<uint8_t*>(base), dev + kDevStreamStrideTableOffset + de.stream))) *
              4;
          std::fprintf(f,
                       "      decl[%u] stream=%u -> slot=%u  offset=%u type=0x%08X "
                       "usage=%u/%u stride=%u dxgi=%u\n",
                       i, de.stream, FetchSlotForStream(de.stream), de.offset, de.type, de.usage,
                       de.usage_index, stride, DeclTypeToDxgi(de.type));
        }
      }
    }
    for (uint32_t v = 0; v < 2; ++v) {
      const uint32_t off = vs_obj ? R32(base, vs_obj + 896 + 8 * v) : 0;
      const ShaderUcodeRef u = ReadVertexShaderUcode(base, vs_obj, v);
      uint32_t patched = 0, total = 0;
      if (u.valid() && u.size_bytes <= kMaxUcodeBytes &&
          IsGuestRangeReadable(u.guest_address, u.size_bytes)) {
        const auto* p = reinterpret_cast<const uint8_t*>(
            rex::memory::GuestPtr(const_cast<uint8_t*>(base), u.guest_address));
        for (const VertexFetchInstr& fi : DecodeVertexFetches(p, u.size_bytes)) {
          ++total;
          if (fi.format != 0 || fi.stride_bytes != 0) {
            ++patched;
          }
        }
      }
      std::fprintf(f, "    variant%u off=0x%X addr=0x%08X size=%u vfetch=%u patched=%u\n", v, off,
                   u.guest_address, u.size_bytes, total, patched);
    }
  }
  std::fprintf(f, "  primitive_type = %u (%s)\n", primitive_type,
               PrimitiveTypeToTopology(primitive_type) ? "native topology"
                                                       : "NEEDS EXPANSION");
  std::fprintf(f, "  element_count  = %u\n", element_count);
  std::fprintf(f, "  start_element  = %u\n", start_element);
  std::fprintf(f, "  base_vertex    = %d\n", base_vertex);
  std::fprintf(f, "  indexed        = %d\n", indexed ? 1 : 0);
  if (!s.complete) {
    // Always print the detail: the interesting case is format 0
    // (kUndefined), and gating on non-zero values hid exactly that.
    std::fprintf(f,
                 "  INCOMPLETE: %s (usage=%u format=%u vfetch_addr=%u stride=%u offset=%u)\n\n",
                 s.failure ? s.failure : "unknown", s.failure_usage, s.failure_format,
                 s.failure_address, s.failure_stride, s.failure_offset);
    ++g_geom_dumped;
    std::fflush(f);
    return;
  }
  if (indexed) {
    std::fprintf(f, "  index_format   = %s\n", s.index_32bit ? "R32_UINT" : "R16_UINT");
    std::fprintf(f, "  index_base     = 0x%08X  bytes=%u\n", s.index_guest_base,
                 s.index_buffer_bytes);
  }
  // Slicing, exactly as the guest would issue it.
  DrawSlicer slicer(primitive_type, element_count);
  DrawSlice slice;
  uint32_t n_slices = 0;
  while (slicer.Next(slice)) {
    if (n_slices < 4) {
      std::fprintf(f, "  slice[%u]       = start %u count %u\n", n_slices, slice.start,
                   slice.count);
    }
    ++n_slices;
  }
  std::fprintf(f, "  slices         = %u\n", n_slices);

  for (size_t i = 0; i < s.streams.size(); ++i) {
    const VertexStream& st = s.streams[i];
    std::fprintf(f, "  stream[%zu] slot=%u guest_base=0x%08X size=%u stride=%u\n", i,
                 st.fetch_slot, st.guest_base, st.guest_size, st.stride);
    if (st.stride) {
      std::fprintf(f, "             vertices_in_range=%u\n", st.guest_size / st.stride);
    }
  }
  for (size_t i = 0; i < s.input_layout.size(); ++i) {
    const InputElement& e = s.input_layout[i];
    std::fprintf(f, "  element[%zu] %s%u slot=%u offset=%u dxgi=%u\n", i, e.semantic_name,
                 e.semantic_index, e.input_slot, e.aligned_byte_offset, e.dxgi_format);
  }
  // --- textures + samplers: the full fetch-slot -> shader-index chain.
  // This proves there is no offset or inverted convention between the Xenos
  // fetch slot, the index the HLSL reads, and the SharedConstants table.
  {
    const uint32_t shadow = dev + kDevFetchShadowOffset;
    uint32_t n_tex = 0;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const uint32_t ea = shadow + slot * kFetchGroupDwords * 4;
      uint32_t d[6];
      for (uint32_t i = 0; i < 6; ++i) {
        d[i] = R32(base, ea + 4 * i);
      }
      if ((d[0] & 0x3u) != 2u) {
        continue;
      }
      const TextureFetch tf = DecodeTextureFetch(d);
      if (!tf.type_valid || tf.width == 0 || tf.height == 0) {
        continue;
      }
      const SamplerDescription sd = DecodeSampler(d);
      // Byte offsets the generated HLSL reads for this slot.
      const uint32_t tex_byte = SharedTextureIndexByteOffset(0, slot);
      const uint32_t smp_byte = SharedSamplerIndexByteOffset(slot);
      std::fprintf(f,
                   "  tex slot=%2u  %ux%u fmt=%u tiled=%u addr=0x%08X\n"
                   "      shader reads Texture2D index at SharedConstants byte %u "
                   "(= c%u.%c)\n"
                   "      shader reads Sampler   index at SharedConstants byte %u "
                   "(= c%u.%c)\n"
                   "      sampler: filt(min/mag/mip)=%u/%u/%u aniso=%u "
                   "clamp(u/v/w)=%u/%u/%u lod_bias=%.3f\n",
                   slot, tf.width, tf.height, tf.format, tf.tiled ? 1u : 0u, tf.base_address,
                   tex_byte, tex_byte / 16, "xyzw"[(tex_byte / 4) % 4], smp_byte, smp_byte / 16,
                   "xyzw"[(smp_byte / 4) % 4], sd.min_filter, sd.mag_filter, sd.mip_filter,
                   sd.aniso_filter, sd.clamp_x, sd.clamp_y, sd.clamp_z,
                   double(sd.lod_bias_raw) / 32.0);
      if (++n_tex >= 6) {
        break;
      }
    }
    if (n_tex == 0) {
      std::fprintf(f, "  tex: none bound\n");
    }
  }
  // --- constants: dirty masks (sampled pre-call) + a sample of the banks
  {
    auto popcount64 = [](uint64_t v) {
      uint32_t n = 0;
      while (v) { v &= v - 1; ++n; }
      return n;
    };
    std::fprintf(f,
                 "  vs_dirty=0x%016llX (%u bits -> %u regs)  "
                 "ps_dirty=0x%016llX (%u bits -> %u regs)\n",
                 (unsigned long long)vs_dirty, popcount64(vs_dirty),
                 popcount64(vs_dirty) * kAluRegistersPerDirtyBit,
                 (unsigned long long)ps_dirty, popcount64(ps_dirty),
                 popcount64(ps_dirty) * kAluRegistersPerDirtyBit);
    // Dump the registers the translated shaders name explicitly, so the
    // native constant state can be compared against the guest bank.
    static std::vector<uint8_t> vs_bank(kAluBankBytes);
    if (ReadConstantBank(base, dev + kDevVsConstantBankOffset, vs_bank.data())) {
      const auto* f4 = reinterpret_cast<const float*>(vs_bank.data());
      const uint32_t named[] = {0, 8, 12};   // gWorld, gWorldViewProj, gViewInverse
      for (uint32_t r : named) {
        std::fprintf(f, "    vs c%-3u = %12.5f %12.5f %12.5f %12.5f\n", r, f4[r * 4 + 0],
                     f4[r * 4 + 1], f4[r * 4 + 2], f4[r * 4 + 3]);
      }
    }
  }
  // --- render state + PSO key
  {
    const GuestRenderState rs = ReadRenderState(base, dev);
    std::fprintf(f,
                 "  rt_format=%u ds_format=%u sample_count=%u edram_mode=%u\n"
                 "  blend=0x%08X color_ctl=0x%08X color_mask=0x%08X depth_ctl=0x%08X "
                 "raster=0x%08X\n"
                 "  depth: enable=%d write=%d func=%u  alpha_test=%d func=%u  "
                 "cull: front=%d back=%d ccw_front=%d\n",
                 ColorRenderTargetFormatToDxgi(rs.color_format),
                 DepthRenderTargetFormatToDxgi(rs.depth_format),
                 SampleCountFromMsaa(rs.msaa_samples), rs.edram_mode, rs.blend_control0,
                 rs.color_control, rs.color_mask, rs.depth_control, rs.pa_su_sc_mode_cntl,
                 rs.depth_enable ? 1 : 0, rs.depth_write ? 1 : 0, rs.depth_func,
                 rs.alpha_test_enable ? 1 : 0, rs.alpha_func, rs.cull_front ? 1 : 0,
                 rs.cull_back ? 1 : 0, rs.front_face_is_cw ? 0 : 1);

    // Shader identities for the key.
    const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
    const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
    uint64_t vs_id = 0, ps_id = 0;
    const ShaderUcodeRef vsr = ReadVertexShaderUcode(
        base, vs_obj, SelectVertexShaderVariant(base, vs_obj, ps_obj));
    if (vsr.valid() && vsr.size_bytes <= kMaxUcodeBytes &&
        IsGuestRangeReadable(vsr.guest_address, vsr.size_bytes)) {
      vs_id = ShaderIdentity(reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
                                 const_cast<uint8_t*>(base), vsr.guest_address)),
                             vsr.size_bytes);
    }
    const ShaderUcodeRef psr = ReadPixelShaderUcode(base, ps_obj);
    if (psr.valid() && psr.size_bytes <= kMaxUcodeBytes &&
        IsGuestRangeReadable(psr.guest_address, psr.size_bytes)) {
      ps_id = ShaderIdentity(reinterpret_cast<const uint8_t*>(rex::memory::GuestPtr(
                                 const_cast<uint8_t*>(base), psr.guest_address)),
                             psr.size_bytes);
    }
    // Spec variants come from the render state: alpha test for PS, and the
    // R11G11B10 normal path for VS (declared by the shader itself).
    const uint32_t ps_spec = rs.alpha_test_enable ? 2u : 0u;
    const PsoKey key = PipelineCache::MakeKey(s, rs, vs_id, ps_id, 0, ps_spec);
    const size_t h = PsoKeyHash{}(key);
    auto it = g_pso_keys.find(h);
    if (it == g_pso_keys.end()) {
      g_pso_keys.emplace(h, g_geom_dumped);
      std::fprintf(f, "  pso_key=0x%016llX  cache=MISS (new)\n", (unsigned long long)h);
    } else {
      ++g_pso_key_hits;
      std::fprintf(f, "  pso_key=0x%016llX  cache=HIT (same as draw %u)\n",
                   (unsigned long long)h, it->second);
    }
    std::fprintf(f, "  vs_id=%016llX ps_id=%016llX ps_spec=%u\n",
                 (unsigned long long)vs_id, (unsigned long long)ps_id, ps_spec);
  }
  for (const UnsuppliedAttribute& u : s.unsupplied) {
    std::fprintf(f, "  unsupplied  %s%u (vfetch_addr=%u) — declaration does not provide it\n",
                 u.semantic_name, u.semantic_index, u.vfetch_address);
  }
  std::fprintf(f, "\n");
  std::fflush(f);
  ++g_geom_dumped;
}

void TelemetryOnFrameEnd() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame;
  if (g_frame % 600 == 0 && g_draws != 0) {
    WriteReports();
  }
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
