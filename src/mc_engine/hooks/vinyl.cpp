// Car vinyls: the bumper layer caps, the GPU readback window the decal texture
// needs outside the Vinyl Editor, and .vgp export / import.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "../online/online_common.h"  // shared guest-memory helpers (IsGuestPtr, ...)

REXCVAR_DEFINE_BOOL(extra_vinyl_layers, false, "MCLA/Garage",
    "Raise the front/rear BUMPER vinyl caps from 16 to 31 layers each "
    "(64/64/64/31/31). Top and side caps stay at 64 because MCLA's vinyl "
    "composite pipeline hard-caps a single surface at 64 layers (fixed-64 "
    "work-area buffers; >64 overflows and crashes). Bumpers stay <=64 and keep "
    "their 5-bit save field, so existing garage/online cars are 100% compatible.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(export_vinyl, false, "MCLA/Garage",
    "Export the current car's vinyl layers to a .vgp file in <exe dir>/vinyls/ "
    "(auto-named vinyl_<car>_<timestamp>.vgp). Toggle ON while in the garage with "
    "a car loaded; it fires once and flips back OFF. Share the file; import later.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(import_vinyl, "", "MCLA/Garage",
    "Import a .vgp vinyl package onto the current car. Type the file name (found "
    "in <exe dir>/vinyls/, with or without the .vgp extension) and press Enter "
    "while in the garage with a car loaded. It overwrites the car's current vinyl "
    "layers and re-composites, then clears the field. Layers past the active "
    "per-surface cap are dropped (enable extra_vinyl_layers first for 31 bumper "
    "slots).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vinyl_auto_readback, true, "MCLA/Garage",
    "Fix: car vinyls only load outside the Vinyl Editor when GPU readback is on "
    "(the game builds the decal texture on the CPU from a GPU composite). This "
    "briefly enables the emulated GPU's resolve readback (d3d12_ or "
    "vulkan_readback_resolve) for ~1.5s around each vinyl (re)composite "
    "(garage entry / edits) so decals appear everywhere, while keeping readback "
    "OFF during racing for full performance. Leave ON.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Vinyl (decal) layer caps. MCLA stores car decals in a fixed 288-slot layer
// array (each layer = 20 bytes) partitioned across 5 surfaces by two parallel
// 5-entry tables in guest .data/.rdata:
//   dword_820510B0 @ 0x820510B0 = per-surface capacity  {64,64,64,16,16}
//   dword_827E9770 @ 0x827E9770 = per-surface start slot {0,64,128,192,208}
// with start[i+1] = start[i] + cap[i]. Every accessor (sub_82393118 &c.) reads
// these live, and the work-area ctor (sub_82370C38) copies the caps + allocates
// per-surface used-buffers of cap[i] bytes ONCE at garage init — so this patch
// must land before the garage is entered (InitHooks, at startup) and requires a
// restart to take effect.
//
// New layout 85/85/86/16/16 (offsets 0/85/170/256/272) sums to exactly 288, so
// it fills the existing array without any reallocation or struct growth. The
// per-surface save COUNT is serialized in bit-width(cap) bits (sub_82395A50 /
// sub_823955C8): 85/86 keep the 7-bit field of 64, and the bumpers stay 16 (5
// bits), so the save/online bitstream layout is byte-identical -> existing
// garage cars still load. Only the raw "UserVinylData" package blob is stored by
// absolute slot; saved packages read shifted until re-saved (garage cars are fine).
void ApplyVinylLayerCaps() {
    if (!REXCVAR_GET(extra_vinyl_layers)) return;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // dword_820510B0 (the capacity table) lives in the XEX read-only data
    // section — the loader maps XEX_SECTION_READONLY_DATA read-only
    // (xex_module.cpp), so a raw store there faults (0xC0000005). Flip the
    // containing page to read/write first. dword_827E9770 is in .data and
    // already writable; unprotecting it too is a harmless no-op. (Switching the
    // base pointer does NOT help — the fault is page protection, not the base.)
    auto make_writable = [&](uint32_t addr) {
        if (auto* heap = mem->LookupHeap(addr)) {
            heap->Protect(addr, 5 * sizeof(uint32_t),
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
        }
    };

    auto write_table = [&](uint32_t addr, const uint32_t (&vals)[5]) {
        auto* p = mem->TranslateVirtual<uint8_t*>(addr);
        for (int i = 0; i < 5; ++i) {
            uint32_t v = vals[i];
            p[i * 4 + 0] = (v >> 24) & 0xFF;  // guest memory is big-endian
            p[i * 4 + 1] = (v >> 16) & 0xFF;
            p[i * 4 + 2] = (v >> 8) & 0xFF;
            p[i * 4 + 3] = v & 0xFF;
        }
    };

    // Per-surface caps. IMPORTANT: MCLA's vinyl COMPOSITE pipeline has a hard
    // 64-layers-per-surface limit — the work-area buffers at wa+48 (malloc 256 =
    // 64 dwords) and wa+52 (malloc 64) are fixed-64 and indexed by layer up to
    // cap[surf] (sub_8236EA38), so any surface with cap > 64 overflows them and
    // crashes on the vinyl-layer menu / regen. So top/sides stay at 64. Bumpers
    // (surfaces 3,4) go 16 -> 31: still <= 64 (composite-safe) and still 5-bit
    // (16..31), so the save bitstream stays identical -> fully compatible.
    static constexpr uint32_t kCaps[5]    = {64, 64, 64, 31, 31};   // dword_820510B0
    static constexpr uint32_t kOffsets[5] = {0, 64, 128, 192, 223}; // dword_827E9770

    make_writable(0x820510B0);
    make_writable(0x827E9770);
    write_table(0x820510B0, kCaps);
    write_table(0x827E9770, kOffsets);

    LARECOMP_APP_INFO(
        "[Vinyl] Raised bumper layer caps to 64/64/64/31/31 (composite-safe, save-compatible).");
}

// ===========================================================================
// Vinyl exporter (phase 1 of custom-package import/export)
// ===========================================================================
//
// Layout (verified in IDA, default.xex): the current car's vinyl layers live in
// a heap "config block" = *(paint + 132); container = block + 64; the layer
// array starts at container + 2244. 5 surfaces (top, side1, side2, front bumper,
// rear bumper). Per-surface capacity table dword_820510B0 @ 0x820510B0, start-
// offset table dword_827E9770 @ 0x827E9770. Layer slot = array + 20*(offset[s]
// + localIdx); a slot is USED when its u16 shape-id at +16 != 0xFFFF. Each layer
// is a fixed 20-byte struct (half-float pos/scale/rot/skew + packed RGBA + flags)
// — copied verbatim, so export/import is lossless within the same game build.

// Current car's vinyl regen args, cached by Hook_CacheVinylPaint at the regen
// entry (sub_8236D850): work-area, paint object, composite texture, player index.
static std::atomic<uint32_t> g_vinyl_wa{0};
static std::atomic<uint32_t> g_vinyl_paint{0};
static std::atomic<uint32_t> g_vinyl_tex{0};
static std::atomic<uint32_t> g_vinyl_player{0};

// Windowed GPU readback around vinyl (re)composites. MCLA builds the car's decal
// texture on the CPU from a GPU composite; without readback the CPU reads stale
// physical RAM, so vinyls only show inside the Vinyl Editor. The global readback
// is held on for ~1.5s around each regen (the composite + CPU copy finish well
// within that), then off again -- so racing keeps full performance. The window
// itself is readback_window.cpp's, shared with photo mode.

// Returns whether the guest vinyl composite is still running. The per-frame
// driver sub_8235AC78 advances a state machine on the car-model flags
// (+604/+605/+606/+607/+608/+6486) until they all clear = done, and each stage
// waits on the work-area "stage pending" byte wa+248. The full composite of all
// surfaces/layers spans many frames, so we hold readback until it's actually
// idle instead of a fixed guess. Self-contained (direct guest reads).
static bool VinylCompositeBusy() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return false;
    auto* mem = rt->memory();
    if (!mem) return false;
    auto rd8 = [&](uint32_t a) -> uint8_t { return *mem->TranslateVirtual<const uint8_t*>(a); };
    auto is_ptr = [](uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; };

    uint32_t wa = g_vinyl_wa.load(std::memory_order_relaxed);
    // wa+248 = a regen stage's GPU work pending; wa+1696 = the composite-busy
    // byte the driver itself gates stages on (sub_8236DB68).
    if (is_ptr(wa) && (rd8(wa + 248) || rd8(wa + 1696))) return true;

    const auto* p = mem->TranslateVirtual<const uint8_t*>(0x8288DCF8);  // current mcCarModel
    uint32_t car = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (!is_ptr(car)) return false;
    // Pipeline-stage flags (sub_8235AC78): full +608/+604/+605/+607/+6486 and the
    // partial-update path +606 (sub_8236D2A0, which we don't hook) — polling here
    // catches every composite trigger, including the last surface / rear bumper.
    return rd8(car + 604) || rd8(car + 605) || rd8(car + 606) || rd8(car + 607) ||
           rd8(car + 608) || rd8(car + 6486);
}

// Called from the regen hook: (re)arm the readback window.
static void ArmVinylReadbackWindow() {
    if (!REXCVAR_GET(vinyl_auto_readback)) return;
    // Generous bridge until the state machine spins up; TickVinylReadbackWindow
    // then keeps it alive for as long as the composite actually runs.
    HoldGlobalReadback(ReadbackClient::kVinyl, 2'000'000'000LL);  // +2s
}

// Called every frame from Patch_DeltaTimePre. Runs regardless of how the
// composite was triggered: whenever the guest state machine is busy it (re)opens
// the readback window; it closes ~1.5s after the composite goes idle. This
// catches partial updates and late/last-surface composites (e.g. rear bumper)
// that the sub_8236D850 hook alone can miss, without leaving readback on during
// racing (flags stay 0 when no vinyl work is queued).
void TickVinylReadbackWindow() {
    if (VinylCompositeBusy() && REXCVAR_GET(vinyl_auto_readback)) {
        HoldGlobalReadback(ReadbackClient::kVinyl, 1'500'000'000LL);  // +1.5s
    }
}

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    g_vinyl_wa.store(static_cast<uint32_t>(r3.u64), std::memory_order_relaxed);
    g_vinyl_paint.store(static_cast<uint32_t>(r4.u64), std::memory_order_relaxed);
    g_vinyl_tex.store(static_cast<uint32_t>(r5.u64), std::memory_order_relaxed);
    g_vinyl_player.store(static_cast<uint32_t>(r6.u64), std::memory_order_relaxed);
    ArmVinylReadbackWindow();
    static std::atomic_bool logged{false};
    if (!logged.exchange(true)) {
        LARECOMP_APP_INFO("[Vinyl] regen hook fired, wa=0x{:08X} paint=0x{:08X} tex=0x{:08X} player={}",
                          static_cast<uint32_t>(r3.u64), static_cast<uint32_t>(r4.u64),
                          static_cast<uint32_t>(r5.u64), static_cast<uint32_t>(r6.u64));
    }
}

namespace {

constexpr uint32_t kVgpMagic = 0x47565852;  // "RXVG"
constexpr uint32_t kVgpVersion = 1;
constexpr uint32_t kVinylSurfaces = 5;
constexpr uint32_t kLayerStride = 20;
constexpr uint32_t kCapsTable = 0x820510B0;
constexpr uint32_t kOffsetsTable = 0x827E9770;

// The guest-memory helpers (IsGuestPtr, GuestRead/Write*, GuestCStr) moved to
// online/online_common.cpp so the online translation units can link them too;
// online_common.h (included above) declares them. They keep external linkage.

// Resolves the current garage car's paint object: the hook-cached value first,
// else the live mcCarModel global (dword_8288DCF8; carModel+20 = paint). Returns
// 0 if no car is loaded. `src` (optional) is set to which source succeeded.
uint32_t ResolveCurrentPaint(rex::memory::Memory* mem, const char** src = nullptr) {
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    if (IsGuestPtr(paint)) { if (src) *src = "hook"; return paint; }
    uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
    if (IsGuestPtr(car_model)) {
        paint = GuestRead32(mem, car_model + 20);
        if (IsGuestPtr(paint)) { if (src) *src = "carmodel"; return paint; }
    }
    return 0;
}

// Reads the car name C-string at *(paint+76)+8; sanitizes to a filename token.
std::string ReadCarName(rex::memory::Memory* mem, uint32_t paint) {
    std::string name;
    uint32_t p76 = GuestRead32(mem, paint + 76);
    if (IsGuestPtr(p76)) {
        uint32_t str_addr = p76 + 8;
        const auto* s = mem->TranslateVirtual<const char*>(str_addr);
        for (int i = 0; i < 48 && s[i]; ++i) {
            char c = s[i];
            name += (std::isalnum(static_cast<unsigned char>(c))) ? c : '_';
        }
    }
    return name.empty() ? "car" : name;
}

std::string TimestampToken() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

}  // namespace

void ExportVinyl() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Prefer the paint cached by the regen hook; fall back to the current
    // mcCarModel global (dword_8288DCF8, set every frame while the car renders;
    // carModel+20 = paint) so export works even if the hook hasn't fired.
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    const char* src = "hook";
    if (!IsGuestPtr(paint)) {
        uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
        if (IsGuestPtr(car_model)) {
            paint = GuestRead32(mem, car_model + 20);
            src = "carmodel";
        }
    }
    if (!IsGuestPtr(paint)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: no car found (hook cache & mcCarModel both empty) "
                           "— be in the garage with the car visible.");
        return;
    }
    LARECOMP_APP_INFO("[Vinyl] Export: paint=0x{:08X} (via {})", paint, src);
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t array = block + 64 + 2244;

    // Serialize: magic, version, carname, then per surface {cap, usedCount,
    // [localIdx u32 + 20 raw layer bytes]...}. Layer bytes are copied verbatim
    // (already big-endian in guest memory) for lossless round-trip.
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF); out.push_back((v >> 24) & 0xFF);
    };

    std::string car = ReadCarName(mem, paint);
    put32(kVgpMagic);
    put32(kVgpVersion);
    put32(static_cast<uint32_t>(car.size()));
    out.insert(out.end(), car.begin(), car.end());
    put32(kVinylSurfaces);

    uint32_t total_layers = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        // Collect used slots first (shape-id != 0xFFFF).
        std::vector<uint32_t> used;
        for (uint32_t k = 0; k < cap; ++k) {
            uint32_t slot = array + kLayerStride * (off + k);
            if (GuestRead16(mem, slot + 16) != 0xFFFF) used.push_back(k);
        }

        put32(cap);
        put32(static_cast<uint32_t>(used.size()));
        for (uint32_t k : used) {
            uint32_t slot = array + kLayerStride * (off + k);
            const auto* p = mem->TranslateVirtual<const uint8_t*>(slot);
            put32(k);
            out.insert(out.end(), p, p + kLayerStride);
            ++total_layers;
        }
    }

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / ("vinyl_" + car + "_" + TimestampToken() + ".vgp");

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        LARECOMP_APP_ERROR("[Vinyl] Export: cannot open {} for writing.", file.string());
        return;
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    LARECOMP_APP_INFO("[Vinyl] Exported {} layers to {}", total_layers, file.string());
}

// Imports a .vgp package (from <exe dir>/vinyls/) onto the current garage car,
// mirroring the game's own package loader (sub_826A2520): for each surface,
// begin-edit (write container+2072=surf, +8004=1), empty every cap slot, drop in
// the imported layers verbatim, end-edit (container+2072=-1); then trigger one
// full regen by replicating sub_8236D850's work-area writes. All pure guest-
// memory writes (the block is a writable heap object) — no guest calls, so this
// is safe to run from the F4/cvar-callback thread.
void ImportVinyl(const std::string& name_in) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Trim surrounding whitespace/quotes from the typed name.
    std::string name = name_in;
    auto trim = [](std::string& s) {
        auto notspace = [](unsigned char c) { return !std::isspace(c) && c != '"'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    };
    trim(name);
    if (name.empty()) return;

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::path file = dir / name;
    if (!std::filesystem::exists(file, ec))
        if (std::filesystem::exists(dir / (name + ".vgp"), ec)) file = dir / (name + ".vgp");
    if (!std::filesystem::exists(file, ec)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: '{}' not found in {}.", name, dir.string());
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); ++it)
            if (it->path().extension() == ".vgp")
                LARECOMP_APP_INFO("[Vinyl]   available: {}", it->path().filename().string());
        return;
    }

    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t o = 0;
    auto rd32 = [&](uint32_t& v) -> bool {
        if (o + 4 > buf.size()) return false;
        v = uint32_t(buf[o]) | (uint32_t(buf[o + 1]) << 8) | (uint32_t(buf[o + 2]) << 16) |
            (uint32_t(buf[o + 3]) << 24);
        o += 4;
        return true;
    };

    uint32_t magic = 0, ver = 0, nlen = 0, nsurf = 0;
    if (!rd32(magic) || magic != kVgpMagic) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is not a RexGlue .vgp (bad magic).",
                           file.filename().string());
        return;
    }
    rd32(ver);
    if (ver != kVgpVersion)
        LARECOMP_APP_INFO("[Vinyl] Import: file format v{} (importer is v{}); reading anyway.",
                          ver, kVgpVersion);
    if (!rd32(nlen) || o + nlen > buf.size()) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is truncated.", file.filename().string());
        return;
    }
    std::string src_car(reinterpret_cast<const char*>(buf.data() + o), nlen);
    o += nlen;
    if (!rd32(nsurf) || nsurf != kVinylSurfaces) {
        LARECOMP_APP_ERROR("[Vinyl] Import: unexpected surface count ({}).", nsurf);
        return;
    }

    // Parse per-surface used layers: {cap, used, [localIdx u32 + 20 raw bytes]...}.
    struct Layer { uint32_t idx; uint8_t bytes[kLayerStride]; };
    std::vector<std::vector<Layer>> surf_layers(kVinylSurfaces);
    for (uint32_t s = 0; s < nsurf; ++s) {
        uint32_t fcap = 0, used = 0;
        if (!rd32(fcap) || !rd32(used)) {
            LARECOMP_APP_ERROR("[Vinyl] Import: truncated surface {}.", s);
            return;
        }
        (void)fcap;  // layers are placed by live caps, not the file's
        for (uint32_t i = 0; i < used; ++i) {
            uint32_t idx = 0;
            if (!rd32(idx) || o + kLayerStride > buf.size()) {
                LARECOMP_APP_ERROR("[Vinyl] Import: truncated layer data (surface {}).", s);
                return;
            }
            Layer L;
            L.idx = idx;
            std::memcpy(L.bytes, buf.data() + o, kLayerStride);
            o += kLayerStride;
            surf_layers[s].push_back(L);
        }
    }

    // Resolve the current car and its writable vinyl block.
    const char* psrc = "?";
    uint32_t paint = ResolveCurrentPaint(mem, &psrc);
    if (!paint) {
        LARECOMP_APP_ERROR("[Vinyl] Import: no car loaded — be in the garage with the car visible.");
        return;
    }
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t container = block + 64;
    uint32_t array = container + 2244;

    uint32_t written = 0, dropped = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        GuestWrite32(mem, container + 2072, s);  // sub_82392538: begin surface edit
        GuestWrite8(mem, container + 8004, 1);

        for (uint32_t k = 0; k < cap; ++k)  // empty every slot first
            GuestWrite16(mem, array + kLayerStride * (off + k) + 16, 0xFFFF);

        for (const auto& L : surf_layers[s]) {
            if (L.idx >= cap) { ++dropped; continue; }  // past the active cap
            auto* p = mem->TranslateVirtual<uint8_t*>(array + kLayerStride * (off + L.idx));
            std::memcpy(p, L.bytes, kLayerStride);
            ++written;
        }

        GuestWrite32(mem, container + 2072, 0xFFFFFFFFu);  // sub_82392548: end surface edit
    }

    // Trigger a full vinyl regen by replicating sub_8236D850(wa, paint, tex,
    // player) with plain memory writes. Work-area comes from the global
    // dword_8288DFC0 (fall back to the hook-cached r3); tex/player are the values
    // the hook captured at the last regen for this car.
    uint32_t wa = GuestRead32(mem, 0x8288DFC0);
    if (!IsGuestPtr(wa)) wa = g_vinyl_wa.load(std::memory_order_relaxed);
    uint32_t tex = g_vinyl_tex.load(std::memory_order_relaxed);
    uint32_t player = g_vinyl_player.load(std::memory_order_relaxed);
    if (IsGuestPtr(wa)) {
        GuestWrite32(mem, wa + 256, tex);     // a3: composite target texture
        GuestWrite8(mem, wa + 248, 1);
        GuestWrite32(mem, wa + 244, paint);   // a2: paint object
        GuestWrite32(mem, wa + 220, 0);
        GuestWrite32(mem, wa + 224, 0);
        GuestWrite8(mem, wa + 20, 1);
        GuestWrite8(mem, wa + 21, 1);
        GuestWrite8(mem, wa + 251, 1);
        GuestWrite32(mem, wa + 16, player);   // a4: player index
    } else {
        LARECOMP_APP_ERROR("[Vinyl] Import: work-area not ready — layers written but not "
                           "re-composited. Nudge a vinyl edit or re-enter the garage.");
    }

    LARECOMP_APP_INFO("[Vinyl] Imported {} layers ({} over-cap dropped) from '{}' (made for '{}') "
                      "onto car paint=0x{:08X} via {}.",
                      written, dropped, file.filename().string(), src_car, paint, psrc);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void ExportVinyl() {}
#endif // REXGLUE_HAS_XEO3_TARGET
