// Unit test of host_helpers.h: the native versions of the game's helpers against (1) plain reference models and
// (2) the game's own recompiled functions, on random inputs.
//
// (1) always runs. Each native function is checked against a byte-level model of what the helper does (a copy,
//     a fill, rows of a copy...) on random sizes, alignments and contents, with guard bytes around every
//     destination, and its refusals are checked: overlap, the physical window, the register blocks, bad counts.
// (2) runs when compiled with -DHH_WITH_ORIGINALS and an include path that holds hh_originals.inc, which tools/host_helpers_extract.py
//     makes from a generated tree (the game's recompiled code, never committed). Every case runs the recompiled function on one
//     copy of a guest arena and the native version on another (the recompiled one for a case the native one
//     declines), then compares the whole arena except the dead stack below r1, and r3 and the non-volatile
//     registers. The tests of the constant-bank flush also require that the native version declines exactly when
//     the recompiled one calls its ring-wrap routine.
//
// Compile (the dev PC; INCLUDE and LIB as perf\scripts\unit-test.ps1 sets them):
//   clang++ -std=c++23 -O1 -msse4.1 -I src/mc_engine/hooks host_helpers_test.cpp                         (1 only)
//   clang++ -std=c++23 -O1 -msse4.1 -DHH_WITH_ORIGINALS -I <dir of hh_originals.inc> -I <rexglue 0.11 install>/include
//           -I src/mc_engine/hooks host_helpers_test.cpp                                                 (1 and 2)
// Exit code 0 = all passed.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "host_helpers.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace host_helpers;

int g_failed = 0;
long g_checks = 0;

#define EXPECT(cond, ...)                                                    \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            if (g_failed <= 20) {                                            \
                std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);  \
                std::printf(__VA_ARGS__);                                    \
                std::printf("\n");                                           \
            }                                                                \
        }                                                                    \
    } while (0)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t U(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(Next() % n); }  // [0, n)
    uint32_t R(uint32_t lo, uint32_t hi) { return lo + U(hi - lo + 1); }               // [lo, hi]
};

// A guest memory: a 4 GiB reservation of which only the pages the cases use are committed, so `base + guest
// address` works for the low work area and the two image data pages the helpers read.
constexpr uint32_t kWorkLo = 0x8000, kWorkHi = 0x60000;  // committed work area
[[maybe_unused]] constexpr uint32_t kStackSp = 0x5F000;                   // r1 of the original; [kDeadLo, kWorkHi) is not compared
constexpr uint32_t kDeadLo = 0x5E000;
constexpr uint32_t kTablePage = 0x827D5000, kTableLen = 0x2000;
constexpr uint32_t kStepPage = 0x827F3000, kStepLen = 0x1000;

struct Arena {
    uint8_t* base = nullptr;
    Arena() {
#ifdef _WIN32
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull + 0x10000, MEM_RESERVE, PAGE_NOACCESS));
        if (base) {
            Commit(kWorkLo, kWorkHi - kWorkLo);
            Commit(kTablePage, kTableLen);
            Commit(kStepPage, kStepLen);
        }
#else
        base = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (base == MAP_FAILED) base = nullptr;
        if (base) {
            Commit(kWorkLo, kWorkHi - kWorkLo);
            Commit(kTablePage, kTableLen);
            Commit(kStepPage, kStepLen);
        }
#endif
        if (!base) {
            std::printf("cannot reserve the guest arena\n");
            std::exit(2);
        }
    }
    void Commit(uint32_t at, uint32_t len) {
#ifdef _WIN32
        VirtualAlloc(base + at, len, MEM_COMMIT, PAGE_READWRITE);
#else
        mprotect(base + at, len, PROT_READ | PROT_WRITE);
#endif
    }
    void CopyFrom(const Arena& o) {
        std::memcpy(base + kWorkLo, o.base + kWorkLo, kWorkHi - kWorkLo);
        std::memcpy(base + kTablePage, o.base + kTablePage, kTableLen);
        std::memcpy(base + kStepPage, o.base + kStepPage, kStepLen);
    }
    bool SameAs(const Arena& o, uint32_t* where = nullptr) const {
        const uint8_t* a = base;
        const uint8_t* b = o.base;
        for (uint32_t i = kWorkLo; i < kDeadLo; ++i) {
            if (a[i] != b[i]) {
                if (where) *where = i;
                return false;
            }
        }
        for (uint32_t i = kTablePage; i < kTablePage + kTableLen; ++i)
            if (a[i] != b[i]) {
                if (where) *where = i;
                return false;
            }
        return true;
    }
    void Randomise(Rng& r) {
        for (uint32_t i = kWorkLo; i < kWorkHi; i += 8) {
            const uint64_t v = r.Next();
            std::memcpy(base + i, &v, 8);
        }
        for (uint32_t i = kTablePage; i < kTablePage + kTableLen; ++i) base[i] = static_cast<uint8_t>(r.Next());
        std::memset(base + kStepPage, 0, kStepLen);
        St32(base, kMemcpyStepAddr, 1);
    }
};

// ============================================================================================ (1) models

void TestRanges() {
    EXPECT(PlainRange(0, 0x1000), "low");
    EXPECT(PlainRange(0x40000000, 0x1000), "heap");
    EXPECT(PlainRange(0x82000000, 0x10000), "image");
    EXPECT(PlainRange(0xBFFFF000, 0x1000), "the end of the 0xA0000000 range");
    EXPECT(!PlainRange(0xDFFFFFF0, 0x20), "crosses the physical window");
    EXPECT(!PlainRange(0xE0000000, 4), "physical window");
    EXPECT(!PlainRange(0x7FC80000, 4), "GPU block");
    EXPECT(!PlainRange(0x7EFFFFFF, 2), "reaches into the register area");
    EXPECT(PlainRange(0x7EFFFFFF, 1), "just below");
    EXPECT(PlainRange(0x80000000, 4), "just above");
    EXPECT(!PlainRange(0xFFFFFFFF, 2), "wraps");
    EXPECT(Disjoint(0, 16, 16, 16) && !Disjoint(0, 17, 16, 16) && Disjoint(100, 0, 100, 16), "disjoint");
}

void TestMemcpyModel(Arena& a, Rng& rng) {
    int declined_overlap = 0;
    for (int it = 0; it < 4000; ++it) {
        a.Randomise(rng);
        uint32_t n = rng.U(8) == 0 ? rng.R(0, 20000) : rng.R(0, 700);
        const uint32_t dst = 0x10000 + rng.U(256);
        const uint32_t src = 0x30000 + rng.U(256);
        if (n > 0x1E000) n = 0x1E000;
        std::vector<uint8_t> before(a.base + dst - 16, a.base + dst + n + 16);
        std::vector<uint8_t> source(a.base + src, a.base + src + n);
        const bool ok = Memcpy(a.base, dst, src, n);
        EXPECT(ok, "memcpy %u -> %u n=%u declined", src, dst, n);
        if (!ok) continue;
        EXPECT(std::memcmp(a.base + dst, source.data(), n) == 0, "memcpy bytes n=%u", n);
        EXPECT(std::memcmp(a.base + dst - 16, before.data(), 16) == 0 && std::memcmp(a.base + dst + n, before.data() + 16 + n, 16) == 0, "memcpy guard n=%u", n);
    }
    // refusals
    a.Randomise(rng);
    EXPECT(!Memcpy(a.base, 0x10000, 0x10008, 64), "overlap forward");
    EXPECT(!Memcpy(a.base, 0x10008, 0x10000, 64), "overlap backward");
    EXPECT(!Memcpy(a.base, 0x10000, 0x10000, 64), "same");
    EXPECT(Memcpy(a.base, 0x10000, 0x10040, 64), "adjacent is fine");
    EXPECT(!Memcpy(a.base, 0xE0000000u, 0x10000, 64), "physical window dst");
    EXPECT(!Memcpy(a.base, 0x10000, 0x7FC80000u, 64), "mmio src");
    EXPECT(Memcpy(a.base, 0x10000, 0x20000, 0), "n = 0 is a no-op");
    St32(a.base, kMemcpyStepAddr, 2);
    EXPECT(!Memcpy(a.base, 0x10000, 0x20000, 8), "a changed step word declines a small copy");
    EXPECT(Memcpy(a.base, 0x10000, 0x20000, 16), "but not a large one");
    (void)declined_overlap;
}

void TestMemsetModel(Arena& a, Rng& rng) {
    for (int it = 0; it < 4000; ++it) {
        a.Randomise(rng);
        const uint32_t n = rng.U(6) == 0 ? rng.R(0, 9000) : rng.R(0, 400);
        const uint32_t dst = 0x10000 + rng.U(256);
        const uint32_t value = static_cast<uint32_t>(rng.Next());
        std::vector<uint8_t> before(a.base + dst - 16, a.base + dst + n + 16);
        const bool ok = Memset(a.base, dst, value, n);
        EXPECT(ok, "memset declined");
        bool all = true;
        for (uint32_t i = 0; i < n; ++i) all &= a.base[dst + i] == (value & 0xFF);
        EXPECT(all, "memset value n=%u", n);
        EXPECT(std::memcmp(a.base + dst - 16, before.data(), 16) == 0 && std::memcmp(a.base + dst + n, before.data() + 16 + n, 16) == 0, "memset guard");
    }
    EXPECT(!Memset(a.base, 0xE0000000u, 1, 4) && !Memset(a.base, 0x7FC80000u, 1, 4) && Memset(a.base, 0x10000, 1, 0), "memset refusals");
}

// ============================================================================================ (2) originals

#ifdef HH_WITH_ORIGINALS

}  // namespace

#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/fpu.h>
#include <rex/ppc/vmx.h>

#include <cassert>
#include <utility>

// What the generated header gives the recompiled functions, for a flat arena without a physical window.
#define REX_FUNC_PROLOGUE() __builtin_assume((reinterpret_cast<size_t>(base) & 0x1F) == 0)
#define REX_PHYS_HOST_OFFSET(addr) 0u
#define REX_RAW_ADDR(x) (base + static_cast<u32>(x))
#define REX_LOAD_U8(x) (*reinterpret_cast<volatile u8*>(base + static_cast<u32>(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*reinterpret_cast<volatile u16*>(base + static_cast<u32>(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*reinterpret_cast<volatile u32*>(base + static_cast<u32>(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*reinterpret_cast<volatile u64*>(base + static_cast<u32>(x)))
#define REX_STORE_U8(x, y) (*reinterpret_cast<volatile u8*>(base + static_cast<u32>(x)) = (y))
#define REX_STORE_U16(x, y) (*reinterpret_cast<volatile u16*>(base + static_cast<u32>(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*reinterpret_cast<volatile u32*>(base + static_cast<u32>(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*reinterpret_cast<volatile u64*>(base + static_cast<u32>(x)) = __builtin_bswap64(y))
#define REX_STACK_LOAD_U8(x) REX_LOAD_U8(x)
#define REX_STACK_LOAD_U16(x) REX_LOAD_U16(x)
#define REX_STACK_LOAD_U32(x) REX_LOAD_U32(x)
#define REX_STACK_LOAD_U64(x) REX_LOAD_U64(x)
#define REX_STACK_STORE_U8(x, y) REX_STORE_U8(x, y)
#define REX_STACK_STORE_U16(x, y) REX_STORE_U16(x, y)
#define REX_STACK_STORE_U32(x, y) REX_STORE_U32(x, y)
#define REX_STACK_STORE_U64(x, y) REX_STORE_U64(x, y)

// The prologue / epilogue helpers of the generated header (r<First>..r31 and lr through r12, below r1).
using rex_gpr_member_ = PPCRegister PPCContext::*;
static constexpr rex_gpr_member_ rex_gpr_members_[18] = {&PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
                                                         &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24, &PPCContext::r25,
                                                         &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
template <unsigned First>
inline void rex_savegprlr(PPCContext& ctx, uint8_t* base, PPCFunc*) {
    for (unsigned n = First; n <= 31; ++n) REX_STORE_U64(ctx.r1.u32 - 8u * (33u - n), (ctx.*rex_gpr_members_[n - 14]).u64);
    REX_STORE_U32(ctx.r1.u32 - 8u, ctx.r12.u32);
}
template <unsigned First>
inline void rex_restgprlr(PPCContext& ctx, uint8_t* base, PPCFunc*) {
    for (unsigned n = First; n <= 31; ++n) (ctx.*rex_gpr_members_[n - 14]).u64 = REX_LOAD_U64(ctx.r1.u32 - 8u * (33u - n));
    ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - 8u);
    ctx.lr = ctx.r12.u64;
}
#define HH_FALLBACKS(N)                                                                  \
    extern "C" void __savegprlr_##N(PPCContext&, u8*) { std::abort(); }                  \
    extern "C" void __restgprlr_##N(PPCContext&, u8*) { std::abort(); }
HH_FALLBACKS(14) HH_FALLBACKS(15) HH_FALLBACKS(16) HH_FALLBACKS(17) HH_FALLBACKS(18) HH_FALLBACKS(19) HH_FALLBACKS(20) HH_FALLBACKS(21) HH_FALLBACKS(22)
HH_FALLBACKS(23) HH_FALLBACKS(24) HH_FALLBACKS(25) HH_FALLBACKS(26) HH_FALLBACKS(27) HH_FALLBACKS(28) HH_FALLBACKS(29) HH_FALLBACKS(30) HH_FALLBACKS(31)

static int g_stub_calls = 0;
extern "C" void hh_stub_82422EF8(PPCContext& ctx, u8*) {
    ++g_stub_calls;  // the ring-wrap routine: the test only counts that the original reached it
    ctx.r3.u64 = ctx.r4.u64;
}

extern "C" void orig_memcpy(PPCContext&, u8*);
extern "C" void orig_memcpy_words(PPCContext&, u8*);
extern "C" void orig_memcpy_blocks(PPCContext&, u8*);
extern "C" void orig_memset(PPCContext&, u8*);
extern "C" void orig_row_write(PPCContext&, u8*);
extern "C" void orig_shader_const_f(PPCContext&, u8*);
extern "C" void orig_flush_bank(PPCContext&, u8*);

#include "hh_originals.inc"

namespace {

// Registers the ABI keeps: r1, r14..r31 (and r2 and the return value r3 for the functions that return one).
bool SameKeptRegs(const PPCContext& x, const PPCContext& y, bool r3) {
    if (x.r1.u64 != y.r1.u64) return false;
    if (r3 && x.r3.u64 != y.r3.u64) return false;
    for (unsigned n = 14; n <= 31; ++n)
        if ((x.*rex_gpr_members_[n - 14]).u64 != (y.*rex_gpr_members_[n - 14]).u64) return false;
    return true;
}

PPCContext MakeCtx(Rng& rng) {
    PPCContext c{};
    c.r1.u64 = kStackSp;
    for (unsigned n = 14; n <= 31; ++n) (c.*rex_gpr_members_[n - 14]).u64 = rng.Next();  // sentinels that must survive
    c.lr = 0x82001234;
    c.r12.u64 = 0x82001234;
    return c;
}

// One case from the initial state `init`: the recompiled function runs on a copy (`ao`), the native one on another (`an`; the recompiled
// one runs there when the native one declines). Returns true if the native one did the work. The native side touches no register, so
// the ABI-kept registers and r3 of the input must equal what the recompiled function left.
template <class NativeFn>
bool Case(const Arena& init, Arena& ao, Arena& an, const PPCContext& in, void (*original)(PPCContext&, u8*), NativeFn native, const char* what, uint64_t id, bool r3_kept = true) {
    ao.CopyFrom(init);
    an.CopyFrom(init);
    PPCContext co = in, cn = in;
    original(co, ao.base);
    const bool did = native(an.base);
    if (!did) original(cn, an.base);
    uint32_t where = 0;
    EXPECT(ao.SameAs(an, &where), "%s case %llu: memory differs at %#x (native %s)", what, static_cast<unsigned long long>(id), where, did ? "did it" : "declined");
    if (did) {
        EXPECT(SameKeptRegs(co, in, r3_kept), "%s case %llu: the recompiled function changed r3 or a kept register (%llx vs %llx)", what, static_cast<unsigned long long>(id),
               static_cast<unsigned long long>(co.r3.u64), static_cast<unsigned long long>(in.r3.u64));
    }
    return did;
}

void DiffMemcpy(Arena& a, Arena& b, Arena& c, Rng& rng) {
    long declined = 0, total = 0;
    for (int it = 0; it < 30000; ++it) {
        a.Randomise(rng);
        uint32_t n;
        switch (rng.U(6)) {
            case 0: n = rng.R(0, 20); break;
            case 1: n = rng.R(0, 300); break;
            case 2: n = rng.R(120, 1100); break;
            case 3: n = rng.R(0, 64); break;
            case 4: n = rng.U(30) == 0 ? rng.R(1000, 30000) : rng.R(100, 600); break;
            default: n = rng.R(0, 700);
        }
        const uint32_t dst = (rng.U(3) == 0 ? 0x10000 : 0x10000 + rng.U(8) * 16) + rng.U(rng.U(2) ? 160 : 16);
        const uint32_t src = 0x32000 + (rng.U(3) == 0 ? 0 : rng.U(160));
        PPCContext in = MakeCtx(rng);
        in.r3.u64 = dst;
        in.r4.u64 = src;
        in.r5.u64 = n;
        ++total;
        const bool did = Case(a, b, c, in, orig_memcpy, [&](u8* base) { return Memcpy(base, dst, src, n); }, "memcpy", it);
        declined += !did;
    }
    EXPECT(declined == 0, "memcpy: %ld of %ld non-overlapping cases declined", declined, total);
    // overlapping cases are only ever declined: the native side never touches them
    for (int it = 0; it < 2000; ++it) {
        a.Randomise(rng);
        const uint32_t n = rng.R(1, 300), dst = 0x10000 + rng.U(64), src = dst + rng.U(n + 1) - (rng.U(2) ? 0 : rng.U(n + 1));
        EXPECT(!Memcpy(a.base, dst, src, n) || !(static_cast<uint64_t>(src) < dst + n && static_cast<uint64_t>(dst) < src + n), "overlap accepted");
    }
}

void DiffMemset(Arena& a, Arena& b, Arena& c, Rng& rng) {
    long declined = 0;
    for (int it = 0; it < 30000; ++it) {
        a.Randomise(rng);
        const uint32_t n = rng.U(5) == 0 ? rng.R(0, 3000) : rng.R(0, 80);
        const uint32_t dst = 0x10000 + rng.U(64);
        const uint32_t value = static_cast<uint32_t>(rng.Next()) & (rng.U(2) ? 0xFFFFFFFFu : 0xFFu);
        PPCContext in = MakeCtx(rng);
        in.r3.u64 = dst;
        in.r4.u64 = value;
        in.r5.u64 = n;
        declined += !Case(a, b, c, in, orig_memset, [&](u8* base) { return Memset(base, dst, value, n); }, "memset", it);
    }
    EXPECT(declined == 0, "memset: %ld declined", declined);
}

// The row writer: an object with the format bytes, a row-pointer table, the pitch table in the image page.
void DiffRowWrite(Arena& a, Arena& b, Arena& c, Rng& rng) {
    long declined = 0, ran = 0;
    for (int it = 0; it < 20000; ++it) {
        a.Randomise(rng);
        // small pitches so the rows fit the work area; some large
        for (uint32_t i = 0; i < 256; ++i) a.base[kRowPitchTable + i] = static_cast<uint8_t>(rng.U(8) == 0 ? rng.R(30, 250) : rng.R(0, 24));
        const uint32_t obj = 0x9000, fmts = 0x9100, ptrcell = 0x9040, rowtab = 0x9200, rowarea = 0x11000, srcarea = 0x40000;
        St32(a.base, obj + 8, fmts);
        const uint32_t slot1 = rng.R(1, 12);
        for (uint32_t s = 0; s < 14; ++s) a.base[fmts + s * 32] = static_cast<uint8_t>(rng.U(256));
        St32(a.base, ptrcell, rowtab);
        for (uint32_t s = 0; s < 14; ++s) St32(a.base, rowtab + s * 4, rowarea + rng.U(2) * 0x4000 + rng.U(64) * 4 + rng.U(3));
        const uint32_t bytes = 4 * rng.R(1, 60);
        const uint32_t rows = rng.R(0, 30) == 0 ? 0 : rng.R(1, 24);
        const uint32_t src = srcarea + rng.U(64);
        PPCContext in = MakeCtx(rng);
        in.r3.u64 = obj;
        in.r4.u64 = ptrcell;
        in.r5.u64 = rng.U(40) == 0 ? 0 : slot1;
        in.r6.u64 = src;
        in.r7.u64 = bytes;
        in.r8.u64 = rows;
        const uint32_t pitch = static_cast<uint32_t>(a.base[kRowPitchTable + a.base[fmts + (slot1 - 1) * 32]]) << 4;
        // keep every row inside the committed work area (the recompiled code does not check): rows * stride fits below srcarea
        const uint32_t row0 = Ld32(a.base, rowtab + (slot1 - 1) * 4);
        const uint64_t last = static_cast<uint64_t>(row0) + static_cast<uint64_t>(rows) * (bytes + pitch + 4096) + bytes;
        if (last >= 0x3E000 || row0 < 0x10000) continue;
        ++ran;
        const bool did = Case(a, b, c, in, orig_row_write, [&](u8* base) { return RowWrite(base, obj, ptrcell, static_cast<uint32_t>(in.r5.u64), src, bytes, rows); }, "row writer", it, /*r3_kept=*/false);
        declined += !did;
    }
    EXPECT(ran > 5000, "row writer: only %ld cases ran", ran);
    EXPECT(declined == 0, "row writer: %ld of %ld declined", declined, ran);
}

void DiffShaderConstF(Arena& a, Arena& b, Arena& c, Rng& rng) {
    long declined = 0;
    for (int it = 0; it < 30000; ++it) {
        a.Randomise(rng);
        const uint32_t dev = 0x10000 + rng.U(2) * 0x10 + (rng.U(5) == 0 ? rng.U(16) : 0);
        const uint32_t reg = rng.R(0, 200);
        const uint32_t count = rng.U(8) == 0 ? rng.R(0, 130) : rng.R(0, 20);
        const uint32_t src = 0x30000 + rng.U(3) * 0x1000 + rng.U(rng.U(2) ? 64 : 1);
        const uint64_t mask = rng.Next();
        PPCContext in = MakeCtx(rng);
        in.r3.u64 = dev;
        in.r4.u64 = reg;
        in.r5.u64 = src;
        in.r6.u64 = count;
        in.r7.u64 = mask;
        declined += !Case(a, b, c, in, orig_shader_const_f, [&](u8* base) { return ShaderConstF(base, dev, reg, src, count, mask); }, "shader const", it);
    }
    EXPECT(declined == 0, "shader constant copy: %ld declined", declined);
}

void DiffFlushBank(Arena& a, Arena& b, Arena& c, Rng& rng) {
    long wrapped = 0, total = 0;
    for (int it = 0; it < 40000; ++it) {
        a.Randomise(rng);
        const uint32_t dev = 0x10000 + rng.U(2) * 0x20;
        const uint32_t ring = 0x20000 + rng.U(4) * 4 + rng.U(8) * 16;
        const uint32_t bank = 0x40000 + rng.U(3) * 0x100 + (rng.U(4) == 0 ? rng.U(16) : 0);
        uint64_t mask;
        switch (rng.U(6)) {
            case 0: mask = 1ull << rng.U(64); break;
            case 1: mask = ~0ull >> rng.U(60); break;
            case 2: mask = (~0ull << rng.U(64)) & rng.Next(); break;
            case 3: mask = 0x5555555555555555ull << rng.U(2); break;
            default: mask = rng.Next() & (rng.U(2) ? rng.Next() : ~0ull);
        }
        if (mask == 0) mask = 1;
        const uint32_t need = 4u * (static_cast<uint32_t>(std::popcount(mask)) * 16 + 40);
        const uint32_t limit = rng.U(4) == 0 ? ring + rng.U(need + 64) : ring + need + rng.U(200);
        St32(a.base, dev + 48, ring);
        St32(a.base, dev + 52, limit);
        PPCContext in = MakeCtx(rng);
        in.r3.u64 = dev;
        in.r4.u64 = mask;
        in.r5.u64 = rng.U(512);
        in.r6.u64 = bank;
        ++total;
        b.CopyFrom(a);
        c.CopyFrom(a);
        PPCContext co = in;
        g_stub_calls = 0;
        orig_flush_bank(co, b.base);
        const bool orig_wrapped = g_stub_calls > 0;
        const bool did = FlushBank(c.base, dev, mask, static_cast<uint32_t>(in.r5.u64), bank);
        EXPECT(did == !orig_wrapped, "flush bank case %d: native %s but the recompiled one %s the ring-wrap routine (mask %llx, room %u)", it, did ? "did it" : "declined",
               orig_wrapped ? "called" : "did not call", static_cast<unsigned long long>(mask), limit - ring);
        if (did && !orig_wrapped) {
            uint32_t where = 0;
            EXPECT(b.SameAs(c, &where), "flush bank case %d: memory differs at %#x (mask %llx ring %x)", it, where, static_cast<unsigned long long>(mask), ring);
            EXPECT(SameKeptRegs(co, in, true), "flush bank case %d: the recompiled function changed r3 or a kept register", it);
        }
        wrapped += orig_wrapped;
    }
    EXPECT(wrapped > 3000 && total - wrapped > 3000, "flush bank: %ld wrapped of %ld, both kinds needed", wrapped, total);
    // an empty mask, an unaligned ring pointer and a bank inside the ring go to the recompiled function
    a.Randomise(rng);
    St32(a.base, 0x10000 + 48, 0x20000);
    St32(a.base, 0x10000 + 52, 0x30000);
    EXPECT(!FlushBank(a.base, 0x10000, 0, 0, 0x40000), "empty mask declines");
    St32(a.base, 0x10000 + 48, 0x20002);
    EXPECT(!FlushBank(a.base, 0x10000, 1, 0, 0x40000), "unaligned ring pointer declines");
    St32(a.base, 0x10000 + 48, 0x20000);
    EXPECT(!FlushBank(a.base, 0x10000, 1ull << 63, 0, 0x20000), "a bank inside the ring declines");
}


// --bench: nanoseconds per call of the recompiled function and of the native one on cache-hot data, for the sizes the game uses. The ratio is
// an upper bound of the gain in the game (the data there is colder, which hits both). Compile the test with -O2 for numbers that mean something.
template <class F>
double TimeNs(F&& f, int calls) {
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; ++i) f(i);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / calls);
    }
    return best;
}

void Bench(Arena& a, Rng& rng) {
    a.Randomise(rng);
    PPCContext ctx = MakeCtx(rng);
    const int calls = 200000;
    auto report = [&](const char* what, double orig, double nat) { std::printf("  %-34s recompiled %7.1f ns   native %7.1f ns   x%.1f\n", what, orig, nat, orig / nat); };
    std::printf("bench (cache-hot, ns per call):\n");
    for (uint32_t n : {4u, 8u, 12u, 16u, 36u, 64u, 128u, 256u, 1024u, 4096u}) {
        for (uint32_t align : {0u, 4u}) {
            const uint32_t dst = 0x10000, src = 0x30000 + align;
            char what[64];
            std::snprintf(what, sizeof what, "memcpy n=%u src+%u", n, align);
            const double o = TimeNs([&](int) { ctx.r3.u64 = dst; ctx.r4.u64 = src; ctx.r5.u64 = n; orig_memcpy(ctx, a.base); }, calls);
            const double v = TimeNs([&](int) { Memcpy(a.base, dst, src, n); }, calls);
            report(what, o, v);
        }
    }
    for (uint32_t n : {16u, 64u, 256u, 1024u}) {
        char what[64];
        std::snprintf(what, sizeof what, "memset n=%u", n);
        const double o = TimeNs([&](int) { ctx.r3.u64 = 0x10001; ctx.r4.u64 = 0; ctx.r5.u64 = n; orig_memset(ctx, a.base); }, calls);
        const double v = TimeNs([&](int) { Memset(a.base, 0x10001, 0, n); }, calls);
        report(what, o, v);
    }
    for (uint32_t bytes : {16u, 64u, 256u}) {
        for (uint32_t rows : {1u, 4u, 16u}) {
            for (uint32_t i = 0; i < 256; ++i) a.base[kRowPitchTable + i] = 40;  // pitch 640
            St32(a.base, 0x9000 + 8, 0x9100);
            St32(a.base, 0x9040, 0x9200);
            St32(a.base, 0x9200, 0x11000);
            char what[64];
            std::snprintf(what, sizeof what, "row writer %u bytes x %u rows", bytes, rows);
            const double o = TimeNs([&](int) { ctx.r3.u64 = 0x9000; ctx.r4.u64 = 0x9040; ctx.r5.u64 = 1; ctx.r6.u64 = 0x40000; ctx.r7.u64 = bytes; ctx.r8.u64 = rows; orig_row_write(ctx, a.base); }, calls / 4);
            const double v = TimeNs([&](int) { RowWrite(a.base, 0x9000, 0x9040, 1, 0x40000, bytes, rows); }, calls / 4);
            report(what, o, v);
        }
    }
    for (uint32_t count : {1u, 4u, 16u, 64u}) {
        char what[64];
        std::snprintf(what, sizeof what, "shader constant copy %u float4", count);
        const double o = TimeNs([&](int) { ctx.r3.u64 = 0x10000; ctx.r4.u64 = 8; ctx.r5.u64 = 0x30001; ctx.r6.u64 = count; ctx.r7.u64 = 1; orig_shader_const_f(ctx, a.base); }, calls / 2);
        const double v = TimeNs([&](int) { ShaderConstF(a.base, 0x10000, 8, 0x30001, count, 1); }, calls / 2);
        report(what, o, v);
    }
    for (uint64_t mask : {0x8000000000000000ull, 0xF000000000000000ull, 0xAAAA000000000000ull, 0xFFFFFFFF00000000ull}) {
        char what[64];
        std::snprintf(what, sizeof what, "constant bank flush mask %016llx", static_cast<unsigned long long>(mask));
        const double o = TimeNs([&](int) { St32(a.base, 0x10000 + 48, 0x20000); St32(a.base, 0x10000 + 52, 0x28000); ctx.r3.u64 = 0x10000; ctx.r4.u64 = mask; ctx.r5.u64 = 0; ctx.r6.u64 = 0x40000; orig_flush_bank(ctx, a.base); }, calls / 2);
        const double v = TimeNs([&](int) { St32(a.base, 0x10000 + 48, 0x20000); St32(a.base, 0x10000 + 52, 0x28000); FlushBank(a.base, 0x10000, mask, 0, 0x40000); }, calls / 2);
        report(what, o, v);
    }
}
#endif  // HH_WITH_ORIGINALS


void TestRowWriteModel(Arena& a, Rng& rng) {
    // Row writer against a model written from the contract.
    for (int it = 0; it < 4000; ++it) {
        a.Randomise(rng);
        for (uint32_t i = 0; i < 256; ++i) a.base[kRowPitchTable + i] = static_cast<uint8_t>(rng.R(0, 24));
        const uint32_t obj = 0x9000, fmts = 0x9100, ptrcell = 0x9040, rowtab = 0x9200;
        St32(a.base, obj + 8, fmts);
        const uint32_t slot1 = rng.R(1, 12);
        for (uint32_t s = 0; s < 14; ++s) a.base[fmts + s * 32] = static_cast<uint8_t>(rng.U(256));
        St32(a.base, ptrcell, rowtab);
        const uint32_t row0 = 0x11000 + rng.U(64) * 4 + rng.U(3);
        St32(a.base, rowtab + (slot1 - 1) * 4, row0);
        const uint32_t bytes = 4 * rng.R(1, 40), rows = rng.R(1, 20), src = 0x40000 + rng.U(64);
        const uint32_t pitch = static_cast<uint32_t>(a.base[kRowPitchTable + a.base[fmts + (slot1 - 1) * 32]]) << 4;
        const int32_t diff = static_cast<int32_t>(pitch - bytes);
        const uint32_t stride = bytes + (static_cast<uint32_t>(diff >> 2) << 2);
        std::vector<uint8_t> expect(a.base + kWorkLo, a.base + kWorkHi);
        uint32_t d = row0;
        for (uint32_t k = 0; k < rows; ++k, d += stride)
            for (uint32_t i = 0; i < bytes; ++i) expect[d + i - kWorkLo] = a.base[src + k * bytes + i];
        const bool ok = RowWrite(a.base, obj, ptrcell, slot1, src, bytes, rows);
        EXPECT(ok, "row write declined");
        EXPECT(ok && std::memcmp(expect.data(), a.base + kWorkLo, kWorkHi - kWorkLo) == 0, "row write result (bytes %u rows %u stride %u)", bytes, rows, stride);
    }
    // refusals and no-ops
    a.Randomise(rng);
    std::vector<uint8_t> snap(a.base + kWorkLo, a.base + kWorkHi);
    St32(a.base, 0x9000 + 8, 0x9100);
    St32(a.base, 0x9040, 0x9200);
    St32(a.base, 0x9200, 0x11000);
    snap.assign(a.base + kWorkLo, a.base + kWorkHi);
    EXPECT(RowWrite(a.base, 0x9000, 0x9040, 0, 0x40000, 16, 4), "slot 0 returns at once");
    EXPECT(RowWrite(a.base, 0x9000, 0x9040, 1, 0x40000, 16, 0), "zero rows returns at once");
    EXPECT(RowWrite(a.base, 0x9000, 0x9040, 1, 0x40000, 16, static_cast<uint32_t>(-3)), "negative rows returns at once");
    EXPECT(!RowWrite(a.base, 0x9000, 0x9040, 1, 0x40000, 6, 2), "a row size that is not a multiple of 4 declines");
    EXPECT(!RowWrite(a.base, 0x9000, 0x9040, 1, 0x40000, 0, 2), "a row size of 0 declines");
    EXPECT(std::memcmp(snap.data(), a.base + kWorkLo, snap.size()) == 0, "the no-ops and refusals wrote nothing");
}

void TestShaderConstModel(Arena& a, Rng& rng) {
    for (int it = 0; it < 4000; ++it) {
        a.Randomise(rng);
        const uint32_t dev = 0x10000 + rng.U(2) * 16, reg = rng.R(0, 100), count = rng.R(0, 40), src = 0x30000 + rng.U(64);
        const uint64_t mask = rng.Next();
        const uint32_t dst = (dev + (std::rotl(reg + 120u, 4) & 0xFFFFFFF0u)) & ~0xFu;
        std::vector<uint8_t> expect(a.base + kWorkLo, a.base + kWorkHi);
        std::memcpy(expect.data() + dst - kWorkLo, a.base + src, count * 16);
        const uint64_t old = Ld64(a.base, dev);  // note: the OR reads the device word after the copy
        uint64_t seen = 0;
        std::memcpy(&seen, expect.data() + dev - kWorkLo, 8);
        seen = std::byteswap(seen);
        (void)old;
        const uint64_t want = seen | mask;
        const uint64_t be = std::byteswap(want);
        std::memcpy(expect.data() + dev - kWorkLo, &be, 8);
        EXPECT(ShaderConstF(a.base, dev, reg, src, count, mask), "declined");
        EXPECT(std::memcmp(expect.data(), a.base + kWorkLo, expect.size()) == 0, "shader constant copy result reg %u count %u", reg, count);
    }
    a.Randomise(rng);
    EXPECT(!ShaderConstF(a.base, 0x10000, 0, 0x10770, 4, 1), "overlapping source declines (the destination starts at 0x10780)");
    EXPECT(!ShaderConstF(a.base, 0xE0000000u, 0, 0x30000, 4, 1), "device in the physical window declines");
}

void TestFlushBankModel(Arena& a, Rng& rng) {
    // The packet layout of a simple case by hand: the ring at 0x20000 (0 mod 16: two NOP pads), runs at bits 0 and 2 of the mask.
    a.Randomise(rng);
    const uint32_t dev = 0x10000, ring = 0x20000, bank = 0x40000;
    St32(a.base, dev + 48, ring);
    St32(a.base, dev + 52, ring + 4096);
    const uint64_t mask = (1ull << 63) | (1ull << 61);
    std::vector<uint8_t> src(a.base + bank, a.base + bank + 64 * 3);
    EXPECT(FlushBank(a.base, dev, mask, 10, bank), "simple flush declined");
    // Run 1: register 10, 16 registers: two pads (the pointer is 0 mod 16), the header word, 64 bytes of bank block 0. The pointer
    // is then 12 mod 16: run 2 (bit 2: registers 10 + 16 + 16 = 42, from bank block 2) gets three pads.
    EXPECT(Ld32(a.base, ring + 4) == kNopWord && Ld32(a.base, ring + 8) == kNopWord, "two pads");
    EXPECT(Ld32(a.base, ring + 12) == ((15u << 16) | 10u), "header 1 %08x", Ld32(a.base, ring + 12));
    EXPECT(std::memcmp(a.base + ring + 16, src.data(), 64) == 0, "payload 1");
    const uint32_t p2 = ring + 12 + 64;  // the pointer after run 1 (0x2004C)
    EXPECT(Ld32(a.base, p2 + 4) == kNopWord && Ld32(a.base, p2 + 8) == kNopWord && Ld32(a.base, p2 + 12) == kNopWord, "three pads");
    EXPECT(Ld32(a.base, p2 + 16) == ((15u << 16) | 42u), "header 2 %08x", Ld32(a.base, p2 + 16));
    EXPECT(std::memcmp(a.base + p2 + 20, src.data() + 128, 64) == 0, "payload 2");
    EXPECT(Ld32(a.base, dev + 48) == p2 + 16 + 64, "final ring pointer %x", Ld32(a.base, dev + 48));
    // A ring that is too short declines and writes nothing.
    a.Randomise(rng);
    St32(a.base, dev + 48, ring);
    St32(a.base, dev + 52, ring + 40);
    std::vector<uint8_t> snap(a.base + kWorkLo, a.base + kWorkHi);
    EXPECT(!FlushBank(a.base, dev, mask, 10, bank) && std::memcmp(snap.data(), a.base + kWorkLo, snap.size()) == 0, "a full ring declines and writes nothing");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    Arena a;
    Rng rng(20261122);
    TestRanges();
    TestMemcpyModel(a, rng);
    TestMemsetModel(a, rng);
    TestRowWriteModel(a, rng);
    TestShaderConstModel(a, rng);
    TestFlushBankModel(a, rng);
#ifdef HH_WITH_ORIGINALS
    {
        Arena b, c;
        DiffMemcpy(a, b, c, rng);
        DiffMemset(a, b, c, rng);
        DiffRowWrite(a, b, c, rng);
        DiffShaderConstF(a, b, c, rng);
        DiffFlushBank(a, b, c, rng);
        std::printf("differential against the recompiled originals: done\n");
        if (argc > 1 && std::string(argv[1]) == "--bench") Bench(a, rng);
    }
#else
    std::printf("(no HH_WITH_ORIGINALS: the recompiled functions were not compared)\n");
#endif
    std::printf("host_helpers_test: %ld checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
