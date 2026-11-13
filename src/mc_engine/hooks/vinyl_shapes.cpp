// Vinyl shape catalog: ShapeIdx -> source name and texture hash, from the shapes
// already resident or by force-loading every one a few at a time.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "../online/online_common.h"  // shared guest-memory helpers (IsGuestPtr, ...)
#include "../texture_dump.h"

REXCVAR_DEFINE_BOOL(dump_vinyl_shapes, false, "MCLA/Garage",
    "Dump the vinyl shape catalog (every ShapeIdx -> source bitmap filename + "
    "texture header) to <exe dir>/vinyls/vinyl_shapes.json. Enter the Vinyl editor "
    "once so the shape library loads, then toggle ON; it fires once and flips back "
    "OFF. Used to build the image->vinyl decomposer's brush set.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(capture_vinyl_shapes, false, "MCLA/Garage",
    "Hands-free: force-load every vinyl shape a few at a time, hash each, and "
    "build the full ShapeIdx->hash map in vinyls/vinyl_shape_hashes.txt (+ .json) "
    "— no manual browsing. Enter the Vinyl editor (main menu, not a picker grid), "
    "then toggle ON; it sweeps all ~894 shapes over a few seconds and logs when "
    "done. Join the hashes to the DDS the texture dumper already wrote.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// mcCarVinylShapeLibrary (dword_8288DFC4) + its shape database (dword_828CD0F8).
// Per raster category cat (0..22): count = lib[3+cat] (dword @ lib+12+4*cat),
// descriptor table = lib[26+cat] (16 bytes/entry: +0 shapeObj, +8 flags), source
// filename = names[local] where names = *(db + 96 + 8*cat). ShapeIdx = cat*1000+local.
constexpr uint32_t kShapeLib = 0x8288DFC4;
constexpr uint32_t kShapeDb = 0x828CD0F8;
constexpr uint32_t kShapeCats = 23;

}  // namespace

// ===========================================================================
// Vinyl shape catalog dump (ShapeIdx -> source name + content hash of the GPU
// texture, matching the SDK texture dumper so the DDS files can be joined).
// Verified in IDA (default.xex, sub_8236E400 / sub_82371368):
//   lib = mcCarVinylShapeLibrary* @ kShapeLib; db = shape DB @ kShapeDb.
//   count[cat]     = lib[3+cat]   (dword @ lib + 12 + 4*cat)
//   descTable[cat] = lib[26+cat]  (16 bytes/entry: +0 shapeObj, +8 flags)
//   refcount[cat]  = *(lib[49+cat]); request-load count per shape at +4*local
//   names[cat]     = *(db + 96 + 8*cat); filename = names[local] (char*)
//   shapeObj+8     = engine tex wrapper; wrapper+0x1C -> D3DTexture; fetch @ +0x1C
// A descriptor is "loaded" when (flags & 0x30000000) == 0x30000000.
// ===========================================================================

struct ShapeRec {
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    std::string fmt;
};

// Resident-shape probe: parse the shape's texture and content-hash its guest
// texels exactly like the SDK dumper. hash == 0 if not loaded / not decodable.
struct ShapeProbe {
    bool loaded = false;
    uint32_t w = 0, h = 0, base = 0, size = 0, pitch = 0;
    bool tiled = false;
    std::string fmt;
    rex::graphics::xenos::TextureFormat format{};
    rex::graphics::xenos::Endian endian{};
    uint64_t hash = 0;
};

static ShapeProbe ProbeShape(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local) {
    ShapeProbe p;
    uint32_t desc_table = GuestRead32(mem, lib + 4 * (26 + cat));
    if (!IsGuestPtr(desc_table)) return p;
    uint32_t desc = desc_table + 16 * local;
    uint32_t shape_obj = GuestRead32(mem, desc + 0);
    uint32_t flags = GuestRead32(mem, desc + 8);
    p.loaded = (flags & 0x30000000u) == 0x30000000u;
    if (!p.loaded || !IsGuestPtr(shape_obj)) return p;
    uint32_t tex = GuestRead32(mem, shape_obj + 8);
    if (!IsGuestPtr(tex)) return p;
    // wrapper+0x20 = real dims (hi16 = width, lo16 = height); the D3DTexture fetch
    // reports width/height minus 1, so use the wrapper's for the DDS name/decode
    // (matches what the SDK dumper wrote for the already-browsed shapes).
    uint32_t dims = GuestRead32(mem, tex + 0x20);
    uint32_t d3dtex = GuestRead32(mem, tex + 0x1C);
    if (!IsGuestPtr(d3dtex)) return p;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.dword_0 = GuestRead32(mem, d3dtex + 0x1C);
    fetch.dword_1 = GuestRead32(mem, d3dtex + 0x20);
    fetch.dword_2 = GuestRead32(mem, d3dtex + 0x24);
    fetch.dword_3 = GuestRead32(mem, d3dtex + 0x28);
    fetch.dword_4 = GuestRead32(mem, d3dtex + 0x2C);
    fetch.dword_5 = GuestRead32(mem, d3dtex + 0x30);
    rex::graphics::TextureInfo ti{};
    if (rex::graphics::TextureInfo::Prepare(fetch, &ti) && ti.width >= 1 && ti.width <= 4096 &&
        ti.height >= 1 && ti.height <= 4096) {
        p.w = dims >> 16;
        p.h = dims & 0xFFFF;
        if (p.w < 1 || p.w > 4096 || p.h < 1 || p.h > 4096) {
            p.w = ti.width;
            p.h = ti.height;
        }
        p.base = ti.memory.base_address;
        p.size = ti.memory.base_size;
        p.pitch = fetch.pitch;
        p.tiled = ti.is_tiled;
        p.format = ti.format;
        p.endian = ti.endianness;
        const auto* finfo = ti.format_info();
        if (finfo && finfo->name) p.fmt = finfo->name;
        const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
        if (bytes && p.size)
            p.hash = mcla::HashGuestTexture(bytes, p.size);
    }
    return p;
}

static std::string ShapeName(rex::memory::Memory* mem, uint32_t db, uint32_t cat, uint32_t local) {
    uint32_t names = GuestRead32(mem, db + 96 + 8 * cat);
    if (!IsGuestPtr(names)) return {};
    return GuestCStr(mem, GuestRead32(mem, names + 4 * local), 128);
}

static std::filesystem::path VinylDir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::map<uint32_t, ShapeRec> LoadShapeRecs(const std::filesystem::path& dir) {
    std::map<uint32_t, ShapeRec> recs;
    std::ifstream mf(dir / "vinyl_shape_hashes.txt");
    uint32_t midx = 0, mw = 0, mh = 0;
    uint64_t mhash = 0;
    std::string mfmt;
    while (mf >> midx >> std::hex >> mhash >> std::dec >> mw >> mh >> mfmt) {
        if (mhash) recs[midx] = ShapeRec{mhash, mw, mh, mfmt};
    }
    return recs;
}

// Writes vinyl_shape_hashes.txt (accumulated ShapeIdx->hash map) and
// vinyl_shapes.json (the full catalog joined to whatever hashes exist so far).
static void SaveShapeManifest(rex::memory::Memory* mem, uint32_t lib, uint32_t db,
                              const std::map<uint32_t, ShapeRec>& recs,
                              const std::filesystem::path& dir) {
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o.push_back('\\');
            o.push_back(c);
        }
        return o;
    };
    std::string json = "{\n  \"shapes\": [\n";
    uint32_t total = 0;
    bool first = true;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            uint32_t idx = cat * 1000 + local;
            std::string name = ShapeName(mem, db, cat, local);
            auto it = recs.find(idx);
            uint64_t hash = it != recs.end() ? it->second.hash : 0;
            uint32_t w = it != recs.end() ? it->second.w : 0;
            uint32_t h = it != recs.end() ? it->second.h : 0;
            std::string fmt = it != recs.end() ? it->second.fmt : std::string();
            char hbuf[19];
            std::snprintf(hbuf, sizeof(hbuf), "0x%016llX", static_cast<unsigned long long>(hash));
            if (!first) json += ",\n";
            first = false;
            json += "    {\"idx\": " + std::to_string(idx) + ", \"cat\": " + std::to_string(cat) +
                    ", \"local\": " + std::to_string(local) + ", \"name\": \"" + esc(name) +
                    "\", \"w\": " + std::to_string(w) + ", \"h\": " + std::to_string(h) +
                    ", \"fmt\": \"" + fmt + "\", \"hash\": \"" + std::string(hbuf) + "\"}";
            ++total;
        }
    }
    json += "\n  ],\n  \"total\": " + std::to_string(total) +
            ",\n  \"mapped\": " + std::to_string(static_cast<uint32_t>(recs.size())) + "\n}\n";

    {
        std::ofstream mf(dir / "vinyl_shape_hashes.txt", std::ios::trunc);
        for (const auto& [rid, r] : recs) {
            char hb[17];
            std::snprintf(hb, sizeof(hb), "%016llx", static_cast<unsigned long long>(r.hash));
            mf << rid << ' ' << hb << ' ' << r.w << ' ' << r.h << ' ' << r.fmt << '\n';
        }
    }
    std::ofstream f(dir / "vinyl_shapes.json", std::ios::binary | std::ios::trunc);
    if (f) f.write(json.data(), static_cast<std::streamsize>(json.size()));
}

// Single-pass dump: hash every currently-resident shape, merge into the
// persistent map, rewrite the manifest. Use after browsing categories.
void DumpVinylShapes() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
        LARECOMP_APP_ERROR("[Vinyl] Shape dump: catalog not ready (lib=0x{:08X} db=0x{:08X}) "
                           "— enter the Vinyl editor once, then retry.", lib, db);
        return;
    }
    std::filesystem::path dir = VinylDir();
    auto recs = LoadShapeRecs(dir);
    uint32_t resident = 0;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            ShapeProbe p = ProbeShape(mem, lib, cat, local);
            if (p.loaded && p.hash) {
                recs[cat * 1000 + local] = ShapeRec{p.hash, p.w, p.h, p.fmt};
                ++resident;
            }
        }
    }
    SaveShapeManifest(mem, lib, db, recs, dir);
    LARECOMP_APP_INFO("[Vinyl] Shapes: {} mapped total ({} resident this pass).", recs.size(),
                      resident);
}

// ---------------------------------------------------------------------------
// Hands-free capture: force-load every shape a few at a time via the game's own
// request-refcount (lib[49+cat][local]), hash it while resident, release it,
// advance. Runs off Patch_DeltaTimePre so async loads have frames to complete.
// Bounds memory to a small sliding window; the hash matches the DDS the SDK
// dumper already wrote, so no re-browsing / re-dumping is needed.
// ---------------------------------------------------------------------------
static std::atomic_bool g_scap_request{false};  // set by cvar callback (any thread)
static bool g_scap_active = false;               // tick-thread only below
static std::vector<std::pair<uint32_t, uint32_t>> g_scap_list;
static std::map<uint32_t, ShapeRec> g_scap_recs;
static size_t g_scap_cursor = 0;
static size_t g_scap_requested = 0;
static int g_scap_wait = 0;
static uint32_t g_scap_captured = 0;
static constexpr size_t kScapWindow = 6;   // max shapes we hold resident at once
static constexpr int kScapMaxWait = 120;   // frames to await one load before skip

void RequestVinylShapeCapture() { g_scap_request.store(true, std::memory_order_relaxed); }

// Adjust a shape's request-refcount by delta (big-endian RMW). +1 asks the
// per-frame loader to load it; -1 lets it be released. Increment (not set) so we
// never clobber the game's own reference for shapes it currently wants.
static void ShapeRefAdjust(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local,
                           int delta) {
    uint32_t arr = GuestRead32(mem, lib + 4 * (49 + cat));
    if (!IsGuestPtr(arr)) return;
    uint32_t a = arr + 4 * local;
    int64_t v = static_cast<int32_t>(GuestRead32(mem, a));
    v += delta;
    if (v < 0) v = 0;
    GuestWrite32(mem, a, static_cast<uint32_t>(v));
}

void TickVinylShapeCapture() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);

    if (g_scap_request.exchange(false, std::memory_order_relaxed)) {
        if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
            LARECOMP_APP_ERROR("[Vinyl] Capture: catalog not ready — enter the Vinyl editor first.");
        } else if (!g_scap_active) {
            g_scap_list.clear();
            for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
                uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
                if (count > 100000) continue;
                for (uint32_t local = 0; local < count; ++local)
                    g_scap_list.emplace_back(cat, local);
            }
            g_scap_recs = LoadShapeRecs(VinylDir());
            g_scap_cursor = 0;
            g_scap_requested = 0;
            g_scap_wait = 0;
            g_scap_captured = 0;
            g_scap_active = true;
            LARECOMP_APP_INFO("[Vinyl] Capture: sweeping {} shapes...", g_scap_list.size());
        }
    }

    if (!g_scap_active) return;
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) return;  // catalog gone; pause this frame

    // Keep a sliding window of load requests ahead of the cursor.
    size_t want = std::min(g_scap_cursor + kScapWindow, g_scap_list.size());
    while (g_scap_requested < want) {
        auto [c, l] = g_scap_list[g_scap_requested];
        ShapeRefAdjust(mem, lib, c, l, +1);
        ++g_scap_requested;
    }

    if (g_scap_cursor < g_scap_list.size()) {
        auto [c, l] = g_scap_list[g_scap_cursor];
        ShapeProbe p = ProbeShape(mem, lib, c, l);
        if (p.loaded && p.hash) {
            g_scap_recs[c * 1000 + l] = ShapeRec{p.hash, p.w, p.h, p.fmt};
            // Force-loaded shapes are never drawn, so the SDK dump-on-sample path
            // misses them. Write the DDS ourselves — same
            // dump/<hash>_<w>x<h>_<fmt>.dds naming, and it skips any file that
            // already exists (already dumped while browsing).
            static const std::filesystem::path s_dump_dir = mcla::TextureDumpDir();
            const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
            if (bytes && p.size)
                mcla::DumpGuestTexture(s_dump_dir, p.hash, p.w, p.h, p.pitch, p.tiled, p.format,
                                       p.endian, bytes, p.size);
            ++g_scap_captured;
            ShapeRefAdjust(mem, lib, c, l, -1);
            ++g_scap_cursor;
            g_scap_wait = 0;
        } else if (++g_scap_wait > kScapMaxWait) {
            ShapeRefAdjust(mem, lib, c, l, -1);  // give up on this one
            ++g_scap_cursor;
            g_scap_wait = 0;
        }
        return;
    }

    SaveShapeManifest(mem, lib, db, g_scap_recs, VinylDir());
    LARECOMP_APP_INFO("[Vinyl] Capture done: {} shapes mapped ({} newly captured this run).",
                      g_scap_recs.size(), g_scap_captured);
    g_scap_active = false;
    g_scap_list.clear();
    g_scap_recs.clear();
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void DumpVinylShapes() {}
void RequestVinylShapeCapture() {}
void TickVinylShapeCapture() {}
#endif // REXGLUE_HAS_XEO3_TARGET
