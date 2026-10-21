// Building display objects inside the game's Flash movies. See swf_dom.h and
//.

#include "swf_dom.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/ppc/function.h>

#include <atomic>
#include <cstring>

#include "hooks/hooks_internal.h"
#include "logging.h"
#include "pause_menu.h"
#include "swf_import.h"
#include "ui_palette.h"

REX_EXTERN(__imp__rex_sub_825ED3A8);  // bind a swfCONTEXT: swf lock + its pools
REX_EXTERN(__imp__rex_sub_821C8F70);  // leave a critical section
REX_EXTERN(__imp__rex_sub_825FC5E0);  // swfINSTANCE::PlaceObject (vtable +0x1C)
REX_EXTERN(__imp__rex_sub_825FA390);  // swfINSTANCE: remove the child at a depth
REX_EXTERN(__imp__rex_sub_825FB348);  // swfINSTANCE draw (recursive)
REX_EXTERN(__imp__rex_sub_821FC588);  // mcUIManager::Update
REX_EXTERN(__imp__rex_sub_8260AAD8);  // swfMATRIX: set from a PlaceObject2 matrix

namespace larecomp::swf {
namespace {

// mcUIManager (*0x8286D804) +60: the movie map, buckets* @+60, u16 count @+64;
// node +0 char* key, +4 u16 key length, +8 movie**, +12 next.
constexpr uint32_t kUiManagerPtr = 0x8286D804;
constexpr uint32_t kMgrMovieMap  = 60;
constexpr uint32_t kMovieContext = 56;
constexpr uint32_t kMovieEnabled = 68;

constexpr uint32_t kCtxFile     = 28;
constexpr uint32_t kCtxRoot     = 32;
constexpr uint32_t kCtxPool     = 40;   // instance pool: +6 u16 free, +12 free list
constexpr uint32_t kCtxInUpdate = 136;  // u8, set while the movie advances

constexpr uint32_t kFileChars     = 24;
constexpr uint32_t kFileCharCount = 50;  // u16 (+28 is the SWF version byte)
constexpr uint32_t kCharType      = 8;

constexpr uint32_t kInstLocalMatrix = 8;    // a b c d tx ty
constexpr uint32_t kInstLocalCxform = 56;   // [R G B A] mult /64, then [R G B A] add *4
constexpr uint32_t kInstTint        = 76;   // u8 R G B, Color.setRGB
constexpr uint32_t kInstAlpha       = 79;   // u8, 255 = opaque, 0 = hidden
constexpr uint32_t kInstMatrixDirty = 81;   // draw rebuilds the matrix from _x/_xscale...
constexpr uint32_t kInstPropX       = 116;  // float _x, from the timeline base (+92)
constexpr uint32_t kInstPropY       = 120;  // float _y, from the timeline base (+96)
constexpr uint32_t kInstDepth       = 166;
constexpr uint32_t kInstDrawn       = 177;
constexpr uint32_t kInstPlacedBy    = 180;  // the timeline command that placed it
constexpr uint32_t kInstFirstChild  = 188;
constexpr uint32_t kInstNext        = 192;
constexpr uint32_t kInstName        = 196;

// What the game's own helpers (sub_825ED4A0, sub_825EDFA0, ...) clear when they
// are done with a context they bound, before leaving the swf lock.
constexpr uint32_t kSwfLock = 0x8290199C;
constexpr uint32_t kBoundGlobals[] = {0x82901F98, 0x82901B08, 0x82901B04, 0x829019C4,
                                      0x828FF98C, 0x82901B0C, 0x828FF974};

uint16_t R16(const uint8_t* base, uint32_t ea) {
    return uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]);
}

bool Plausible(uint32_t ea) { return ea >= 0x10000u && ea < 0xC0000000u; }

bool NameIs(const uint8_t* base, uint32_t ea, const char* name) {
    if (!Plausible(ea)) return false;
    const size_t n = std::strlen(name);
    return std::memcmp(base + ea, name, n) == 0 && base[ea + n] == 0;
}

std::atomic<uint32_t> g_raw[kMaxRaw] = {};
std::atomic<int> g_raw_count{0};
thread_local int t_raw_depth = 0;

}  // namespace

bool FindMovie(const uint8_t* base, const char* name, Movie& out) {
    out = Movie{};
    const uint32_t mgr = ReadGuestU32(base, kUiManagerPtr);
    if (!Plausible(mgr)) return false;
    const uint32_t buckets = ReadGuestU32(base, mgr + kMgrMovieMap);
    const uint16_t count = R16(base, mgr + kMgrMovieMap + 4);
    for (uint32_t i = 0; Plausible(buckets) && i < count; ++i) {
        for (uint32_t node = ReadGuestU32(base, buckets + 4 * i), guard = 0;
             Plausible(node) && guard < 256; node = ReadGuestU32(base, node + 12), ++guard) {
            if (!R16(base, node + 4) || !NameIs(base, ReadGuestU32(base, node), name)) continue;
            const uint32_t holder = ReadGuestU32(base, node + 8);
            const uint32_t entry = Plausible(holder) ? ReadGuestU32(base, holder) : 0;
            const uint32_t ctx = Plausible(entry) ? ReadGuestU32(base, entry + kMovieContext) : 0;
            if (!Plausible(ctx)) return false;
            out.entry = entry;
            out.ctx = ctx;
            out.file = ReadGuestU32(base, ctx + kCtxFile);
            out.root = ReadGuestU32(base, ctx + kCtxRoot);
            out.enabled = base[entry + kMovieEnabled] != 0;
            return Plausible(out.file) && Plausible(out.root);
        }
    }
    return false;
}

uint32_t FindChild(const uint8_t* base, uint32_t parent, const char* name) {
    if (!Plausible(parent)) return 0;
    for (uint32_t c = ReadGuestU32(base, parent + kInstFirstChild), guard = 0;
         Plausible(c) && guard < 4096; c = ReadGuestU32(base, c + kInstNext), ++guard)
        if (NameIs(base, ReadGuestU32(base, c + kInstName), name)) return c;
    return 0;
}

uint32_t ChildAtDepth(const uint8_t* base, uint32_t parent, uint16_t depth) {
    if (!Plausible(parent)) return 0;
    for (uint32_t c = ReadGuestU32(base, parent + kInstFirstChild), guard = 0;
         Plausible(c) && guard < 4096; c = ReadGuestU32(base, c + kInstNext), ++guard) {
        const uint16_t d = R16(base, c + kInstDepth);
        if (d == depth) return c;
        if (d > depth) break;  // the list is kept sorted by depth
    }
    return 0;
}

uint32_t Character(const uint8_t* base, uint32_t file, int index, uint8_t type) {
    if (!Plausible(file) || index < 0) return 0;
    const uint32_t table = ReadGuestU32(base, file + kFileChars);
    if (!Plausible(table) || index >= R16(base, file + kFileCharCount)) return 0;
    const uint32_t ch = ReadGuestU32(base, table + 4 * uint32_t(index));
    return Plausible(ch) && base[ch + kCharType] == type ? ch : 0;
}

int PoolFree(const uint8_t* base, uint32_t ctx) {
    return Plausible(ctx) ? R16(base, ctx + kCtxPool + 6) : 0;
}

Bind::Bind(uint8_t* base, uint32_t movie_ctx) : base_(base), ctx_(movie_ctx) {
    if (!Plausible(ctx_) || base_[ctx_ + kCtxInUpdate]) return;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_825ED3A8, ctx_, 1u);
    bound_ = true;
}

Bind::~Bind() {
    if (!bound_ || base_[ctx_ + kCtxInUpdate]) return;
    for (const uint32_t g : kBoundGlobals) WriteGuestU32(base_, g, 0);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_821C8F70, kSwfLock);
}

uint32_t Place(uint8_t* base, uint32_t parent, uint16_t depth, uint32_t character) {
    if (!Plausible(parent) || !Plausible(character)) return 0;
    const uint32_t inst = rex::ppc::GuestToHostFunction<uint32_t>(
        __imp__rex_sub_825FC5E0, parent, uint32_t(depth), 0u, 0u, character);
    if (!Plausible(inst)) return 0;
    // What attachMovie (sub_825F9B58) sets on its instances: drawn, and placed by
    // no timeline command, so no RemoveObject2 of the parent's frames targets it.
    base[inst + kInstDrawn] = 1;
    WriteGuestU32(base, inst + kInstPlacedBy, 0);
    // The ctor leaves this byte as the pool slot's last owner left it; set, the
    // first draw would rebuild the matrix from the untouched _x/_xscale fields.
    base[inst + kInstMatrixDirty] = 0;
    return inst;
}

void Remove(uint8_t* base, uint32_t parent, uint16_t depth) {
    (void)base;
    if (!Plausible(parent)) return;
    rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_825FA390, parent, uint32_t(depth));
}

void SetMatrix(uint8_t* base, uint32_t inst, float a, float b, float c, float d, float tx,
               float ty) {
    const float m[6] = {a, b, c, d, tx, ty};
    for (int i = 0; i < 6; ++i) WriteGuestF32(base, inst + kInstLocalMatrix + 4 * i, m[i]);
    base[inst + kInstMatrixDirty] = 0;
}

void GetMatrix(const uint8_t* base, uint32_t inst, float m[6]) {
    for (int i = 0; i < 6; ++i) m[i] = ReadGuestF32(base, inst + kInstLocalMatrix + 4 * i);
}

void SetSolidColor(uint8_t* base, uint32_t inst, uint32_t rgb, uint8_t alpha) {
    // Multipliers are 6-bit fractions (64 = 1.0) applied to the art's color, so
    // white art comes out as the color itself, to within 255/64 per channel.
    auto mul = [](uint32_t c) { return uint8_t((c * 64 + 127) / 255); };
    uint8_t* cx = base + inst + kInstLocalCxform;
    // R G B A, as sub_82607AF8 reads them (index 0 multiplies the fill's red;
    // the decompiler's BYTE1/BYTE2 names have the two swapped).
    cx[0] = mul((rgb >> 16) & 0xFF);  // R
    cx[1] = mul((rgb >> 8) & 0xFF);   // G
    cx[2] = mul(rgb & 0xFF);          // B
    cx[3] = mul(alpha);               // A
    cx[4] = cx[5] = cx[6] = cx[7] = 0;
}

void CopyColor(uint8_t* base, uint32_t to, uint32_t from) {
    std::memcpy(base + to + kInstLocalCxform, base + from + kInstLocalCxform, 8);
}

void KeepDrawn(uint8_t* base, uint32_t inst) { base[inst + kInstDrawn] = 1; }

void SetVisible(uint8_t* base, uint32_t inst, bool on) {
    base[inst + kInstAlpha] = on ? 0xFF : 0x00;
}

void SetAlphaPercent(uint8_t* base, uint32_t inst, int percent) {
    base[inst + kInstAlpha] = uint8_t(255 * percent / 100);
}

void SetX(uint8_t* base, uint32_t inst, float x) {
    WriteGuestF32(base, inst + kInstPropX, x);
    base[inst + kInstMatrixDirty] = 1;
}

void SetY(uint8_t* base, uint32_t inst, float y) {
    WriteGuestF32(base, inst + kInstPropY, y);
    base[inst + kInstMatrixDirty] = 1;
}

void SetTint(uint8_t* base, uint32_t inst, uint32_t rgb) {
    base[inst + kInstTint + 0] = uint8_t(rgb >> 16);
    base[inst + kInstTint + 1] = uint8_t(rgb >> 8);
    base[inst + kInstTint + 2] = uint8_t(rgb);
}

void SetBaseTranslation(uint8_t* base, uint32_t inst, float tx, float ty) {
    static uint32_t s_matrix = 0;  // a b c d tx ty, the argument it reads
    if (!s_matrix) s_matrix = GuestScratch(24);
    if (!s_matrix) return;
    const float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, tx, ty};
    for (int i = 0; i < 6; ++i) WriteGuestF32(base, s_matrix + 4 * i, m[i]);
    rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_8260AAD8, inst + kInstLocalMatrix,
                                        s_matrix);
}

void SetRaw(const uint32_t* insts, int count) {
    if (count > kMaxRaw) count = kMaxRaw;
    g_raw_count.store(0, std::memory_order_release);
    for (int i = 0; i < count; ++i) g_raw[i].store(insts[i], std::memory_order_relaxed);
    g_raw_count.store(count > 0 ? count : 0, std::memory_order_release);
}

bool DrawingRaw() { return t_raw_depth > 0; }

}  // namespace larecomp::swf

// Every instance of every movie is drawn through here (the recursion goes
// through the overridable symbol too). Ours are marked so the theme remap in
// hooks/ui_theme.cpp leaves the colors they were given alone.
extern "C" REX_FUNC(rex_sub_825FB348) {
    using namespace larecomp::swf;
    bool raw = false;
    const int n = g_raw_count.load(std::memory_order_acquire);
    for (int i = 0; i < n && !raw; ++i)
        raw = g_raw[i].load(std::memory_order_relaxed) == ctx.r3.u32;
    if (raw) ++t_raw_depth;
    __imp__rex_sub_825FB348(ctx, base);
    if (raw) --t_raw_depth;
}

// mcUIManager::Update: input, the vhsm and every movie's step. Display objects
// of our own are kept up right after it, on the same thread and at the same
// point of the frame as the timelines' own placements.
extern "C" REX_FUNC(rex_sub_821FC588) {
    static bool s_live_logged = false;  // only the game thread runs the UI update
    if (!s_live_logged) {
        s_live_logged = true;
        MC_INFO("[ui-palette] UI tick live (mcUIManager::Update wrapped)");
    }
    __imp__rex_sub_821FC588(ctx, base);
    const uint64_t r3 = ctx.r3.u64;
    PauseMenuFormsTick();
    larecomp::UiPaletteTick(base);
    ctx.r3.u64 = r3;
}

#endif  // REXGLUE_HAS_XEO3_TARGET
