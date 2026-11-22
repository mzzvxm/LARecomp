// Unit test of race_line_native.h: the native backward and forward speed passes of the AI racers' driving line
// against the game's own recompiled functions, on random inputs and (when a recording exists) on recorded jobs.
//
// (1) always runs: the arithmetic helpers against the standard library's (fma, the truncating conversion), the address
//     rules, the constants' addresses against a seeded image page; and (race_line_cap.h) the rate cap's clock and schedule: jobs per second of GAME
//     time under a fixed step at any wall FPS, the steady clock without one, a clock that steps back, a changed layout.
// (2) runs when compiled with -DRL_WITH_ORIGINALS and an include path that holds rl_originals.inc, which tools/race_line_extract.py
//     makes from a generated tree (the game's recompiled code, never committed). Each random case builds a planner tuning block, a
//     vehicle tuning block and a path (up to 66 samples of 56 bytes) in a guest arena, runs the recompiled function on one copy and
//     the native one on another, and compares every byte of the arena except the dead stack below r1, plus the result in f1. The
//     chain case runs both passes one after the other (the backward pass's result is the forward pass's argument). A mutation check
//     (a wrong constant) must be caught.
// (3) with -DRL_WITH_ORIGINALS and `--replay <file> --image <image.bin>`: the whole "UpdateRaceLine" job (sub_82171078 and its 29 callees,
//     rl_originals.inc) runs twice on each job recorded by the game (race_line_record, format at the top of hooks/race_line.cpp):
//     once as the recompiled code, once with its two passes replaced by the native ones; every byte of the guest memory (the path
//     except the elapsed-time word at path + 5360, the scratch block, the tuning blocks) must come out the same. `--replay-parse`
//     only reads the file and prints what is in it (no originals needed).
// `--bench` times the recompiled and the native passes on the same data (needs the originals).
// `--image <image.bin>` loads the game's image (C:\mcv\larecomp\native\work\image.bin) into the arena at 0x82000000 instead of the seeded
// constants and checks that the seeds equal the image's values.
//
// Compile (the dev PC; INCLUDE and LIB as perf\scripts\unit-test.ps1 sets them; -mfma is not needed, the header selects it per function):
//   clang++ -std=c++23 -O2 -msse4.1 -I src/mc_engine/hooks race_line_test.cpp                                      (1 only)
//   clang++ -std=c++23 -O2 -msse4.1 -DRL_WITH_ORIGINALS -I <dir of rl_originals.inc> -I <rexglue 0.11 install>/include
//           -I src/mc_engine/hooks race_line_test.cpp                                                             (1, 2 and 3)
// Exit code 0 = all passed.

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "race_line_cap.h"
#include "race_line_native.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace race_line;

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
    double D() { return static_cast<double>(Next() >> 11) / 9007199254740992.0; }      // [0, 1)
    double D(double lo, double hi) { return lo + (hi - lo) * D(); }
};

// ---------------------------------------------------------------------------------------------- the guest arena

// Plain guest addresses the random cases use (only the test's numbers): the planner block and the path are neighbours, as in the game
// (path = block + 25424), the vehicle block and the scratch block are elsewhere, the stack is the top of the work area.
constexpr uint32_t kCtx = 0x10000, kCtxLen = 25424;
constexpr uint32_t kPath = kCtx + kCtxLen, kPathLen = 5376;
constexpr uint32_t kVeh = 0x20000, kVehLen = 8780;
constexpr uint32_t kScratch = 0x30000, kScratchLen = 21600;
constexpr uint32_t kWorkLo = 0x10000, kWorkHi = 0x80000;
constexpr uint32_t kStackSp = 0x7F000;  // r1; [kDeadLo, kStackSp) is the dead stack below r1: not compared
constexpr uint32_t kDeadLo = 0x7E000;
constexpr uint32_t kImage = 0x82000000u;
constexpr uint32_t kElapsedWord = 5360;  // path + 5360: the job's elapsed time (host clock: differs between two runs)

// A job that touches memory the recording did not hold faults; say which guest address, so the recording's blocks can be widened.
uint8_t* g_arena_bases[8] = {};
int g_arena_count = 0;
#ifdef _WIN32
LONG WINAPI GuestFault(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const uint8_t* at = reinterpret_cast<const uint8_t*>(ep->ExceptionRecord->ExceptionInformation[1]);
        for (int i = 0; i < g_arena_count; ++i)
            if (at >= g_arena_bases[i] && at < g_arena_bases[i] + 0x100010000ull) {
                std::printf("FAULT: the code touched guest address %#llx (%s), which is not in the recording or the work area\n", static_cast<unsigned long long>(at - g_arena_bases[i]),
                            ep->ExceptionRecord->ExceptionInformation[0] ? "a write" : "a read");
                std::fflush(stdout);
                std::exit(3);
            }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

struct Arena {
    uint8_t* base = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> blocks;  // committed [lo, hi), page aligned
    Arena() {
#ifdef _WIN32
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull + 0x10000, MEM_RESERVE, PAGE_NOACCESS));
#else
        base = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (base == MAP_FAILED) base = nullptr;
#endif
        if (!base) {
            std::printf("cannot reserve the guest arena\n");
            std::exit(2);
        }
        if (g_arena_count < 8) g_arena_bases[g_arena_count++] = base;
    }
    void Commit(uint32_t lo, uint32_t hi) {
        lo &= ~0xFFFu;
        hi = (hi + 0xFFFu) & ~0xFFFu;
        for (auto& b : blocks)
            if (lo >= b.first && hi <= b.second) return;
#ifdef _WIN32
        VirtualAlloc(base + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE);
#else
        mprotect(base + lo, hi - lo, PROT_READ | PROT_WRITE);
#endif
        blocks.emplace_back(lo, hi);
    }
    void CopyFrom(const Arena& o) {
        for (auto& b : o.blocks) {
            Commit(b.first, b.second);
            std::memcpy(base + b.first, o.base + b.first, b.second - b.first);
        }
    }
    // Same bytes everywhere that is committed, except [kDeadLo, kStackSp) and the words at the given addresses. In [nan_lo, nan_hi) (the
    // path: floats only) two words that are both NaN count as equal: which NaN comes out of an invalid operation (the sign, the payload)
    // depends on the order the compiler gave the operands, and real data has none; `nans` counts them.
    bool SameAs(const Arena& o, uint32_t* where, const uint32_t* skip_words = nullptr, size_t nskip = 0, uint32_t nan_lo = 0, uint32_t nan_hi = 0, long* nans = nullptr) const {
        for (auto& b : blocks) {
            for (uint32_t i = b.first; i < b.second; ++i) {
                if (i >= kDeadLo && i < kStackSp) continue;
                if (base[i] == o.base[i]) continue;
                bool skipped = false;
                for (size_t k = 0; k < nskip; ++k)
                    if (i >= skip_words[k] && i < skip_words[k] + 4) skipped = true;
                if (skipped) continue;
                if (i >= nan_lo && i < nan_hi) {
                    const uint32_t w = i & ~3u;
                    const float x = std::bit_cast<float>(Ld32(base, w)), y = std::bit_cast<float>(Ld32(o.base, w));
                    if (std::isnan(x) && std::isnan(y)) {
                        if (nans) ++*nans;
                        i = w + 3;
                        continue;
                    }
                }
                *where = i;
                return false;
            }
        }
        return true;
    }
};

inline void PutF(Arena& a, uint32_t addr, float f) { St32(a.base, addr, std::bit_cast<uint32_t>(f)); }

// The constants of the two functions at their image addresses.
void SeedConstants(Arena& a) {
    a.Commit(kImage, kImage + 0x9000);
    a.Commit(0x82040000u, 0x82041000u);
    const float fs[8] = {0.1f, 0.0f, 2.0f, 1.0f, 126.1f, -8388608.0f, 8388608.0f, 1e-6f};
    for (int i = 0; i < 8; ++i) PutF(a, kConstAddr[i], fs[i]);
    const double ds[2] = {0.5, 1.0};
    for (int i = 0; i < 2; ++i) {
        const uint64_t v = __builtin_bswap64(std::bit_cast<uint64_t>(ds[i]));
        std::memcpy(a.base + kConstDoubleAddr[i], &v, 8);
    }
}

bool LoadImage(Arena& a, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    a.Commit(kImage, kImage + static_cast<uint32_t>(n));
    const size_t got = std::fread(a.base + kImage, 1, static_cast<size_t>(n), f);
    std::fclose(f);
    return got == static_cast<size_t>(n);
}

// ---------------------------------------------------------------------------------------------- (1) reference checks

#if RL_HAS_X86
RL_ENTRY long CountFmaDifferences() {
    Rng rng(11);
    long bad = 0;
    for (int i = 0; i < 200000; ++i) {
        const double a = rng.D(-1e3, 1e3), b = rng.D(-1e3, 1e3), c = rng.D(-1e6, 1e6);
        if (std::bit_cast<uint64_t>(Fma(a, b, c)) != std::bit_cast<uint64_t>(std::fma(a, b, c))) ++bad;
    }
    return bad;
}
#endif

void TestHelpers(const char* image) {
#if RL_HAS_X86
    if (!Fma3Available()) {
        std::printf("note: this CPU has no FMA3: the native passes are not exercised\n");
        return;
    }
    const long bad = CountFmaDifferences();
    EXPECT(bad == 0, "Fma differs from std::fma in %ld of 200000 cases", bad);
    EXPECT(Ctiwz(3.99) == 3 && Ctiwz(-3.99) == -3 && Ctiwz(0.0) == 0, "ctiwz truncates");
    EXPECT(Ctiwz(3e9) == INT32_MAX && Ctiwz(-3e9) == INT32_MIN && Ctiwz(std::nan("")) == INT32_MIN, "ctiwz saturates");
    EXPECT(S(0.1) == static_cast<double>(0.1f) && S(1.0e300) == INFINITY, "S rounds to float");
    EXPECT(PlainBlock(0x10000, 100) && !PlainBlock(0, 100) && !PlainBlock(0x7EFFFFF0u, 0x20) && PlainBlock(0x7EFFFFF0u, 0x10) && !PlainBlock(0x7FC80000u, 4) && PlainBlock(0x82000000u, 0x1000) &&
               PlainBlock(0xDFFFFFF0u, 0x10) && !PlainBlock(0xDFFFFFF0u, 0x20) && !PlainBlock(0xE0000000u, 4) && !PlainBlock(0xFFFFFFF0u, 0x20) && !PlainBlock(0x20000, 0),
           "PlainBlock");
    Arena a;
    SeedConstants(a);
    const Constants k = Constants::Load(a.base);
    EXPECT(k.f[0] == static_cast<double>(0.1f) && k.f[2] == 2.0 && k.f[3] == 1.0 && k.f[4] == static_cast<double>(126.1f) && k.d[0] == 0.5 && k.d[1] == 1.0, "seeded constants");
    if (image) {
        Arena b;
        if (!LoadImage(b, image)) {
            EXPECT(false, "cannot read the image %s", image);
        } else {
            const Constants ki = Constants::Load(b.base);
            bool same = true;
            for (int i = 0; i < 8; ++i) same = same && std::bit_cast<uint64_t>(ki.f[i]) == std::bit_cast<uint64_t>(k.f[i]);
            for (int i = 0; i < 2; ++i) same = same && std::bit_cast<uint64_t>(ki.d[i]) == std::bit_cast<uint64_t>(k.d[i]);
            EXPECT(same, "the image's constants are not the seeded ones");
        }
    }
#else
    (void)image;
#endif
}

// ---------------------------------------------------------------------------------------------- the rate cap

// The jobs one planner (its layout never changing) gets in `game_seconds` of game time, update by update: the game moves `step_fps` steps a second
// when a fixed step is set (0: none, and the game then moves with the wall clock), the wall clock `wall_fps` updates a second, the cap is `cap_hz`.
long CapJobs(double step_fps, double wall_fps, int cap_hz, double game_seconds) {
    const uint64_t ticks = step_fps > 0.0 ? static_cast<uint64_t>(std::llround(1.0 / step_fps / mc::cinematic::capture::kGameTickSeconds)) : 0;
    const double game_dt = step_fps > 0.0 ? 1.0 / step_fps : 1.0 / wall_fps;
    const int64_t steady0 = 5'000'000'000'000ll;  // a PC that has run for a while: the steady clock's origin is not zero
    const int64_t interval = 1'000'000'000ll / cap_hz;
    race_line::RateSchedule schedule;
    long jobs = 0;
    const uint64_t updates = static_cast<uint64_t>(game_seconds / game_dt);
    for (uint64_t u = 1; u <= updates; ++u) {
        const int64_t steady = steady0 + static_cast<int64_t>(static_cast<double>(u) * 1e9 / wall_fps);
        const int64_t now = race_line::CapClockNs(ticks, u, steady);
        if (u == 1) {
            schedule.Start(now, interval);
            ++jobs;
        } else if (schedule.Due(true, now, interval)) {
            ++jobs;
        }
    }
    return jobs;
}

void TestRateCap() {
    namespace cap = mc::cinematic::capture;
    // the clock: 60 updates of the 60 fps step (831250 ticks) are one second of game time; no step: the steady clock as it is
    EXPECT(std::llround(1.0 / 60.0 / cap::kGameTickSeconds) == 831250, "the 60 fps step is 831250 ticks");
    const int64_t one_second = race_line::CapClockNs(831250, 60, 123);
    EXPECT(one_second > 999'990'000ll && one_second < 1'000'010'000ll, "60 updates of the 60 fps step are %lld ns", static_cast<long long>(one_second));
    EXPECT(race_line::CapClockNs(0, 60, 123456789ll) == 123456789ll, "no fixed step: the steady clock");

    // jobs per GAME second over 20 game seconds (a step of 1/30 s at 240 wall FPS is the dev fixed-step mode the cap used to get wrong: the game runs
    // 8 times faster than the wall clock, and on the wall clock "30 a second" was one job per 8 updates)
    struct Case {
        const char* what;
        double step_fps, wall_fps;
        int cap_hz;
        long expect;
    };
    const Case cases[] = {
        {"fixed step 30, 240 wall FPS, cap 30", 30.0, 240.0, 30, 600},
        {"fixed step 30, 240 wall FPS, cap 12", 30.0, 240.0, 12, 240},
        {"fixed step 60, 20 wall FPS (a slow capture), cap 30", 60.0, 20.0, 30, 600},
        {"fixed step 144, 30 wall FPS, cap 30", 144.0, 30.0, 30, 600},
        {"no step, 144 wall FPS, cap 30 (normal play)", 0.0, 144.0, 30, 600},
        {"no step, 60 wall FPS, cap 30 (normal play)", 0.0, 60.0, 30, 600},
    };
    for (const Case& c : cases) {
        const long jobs = CapJobs(c.step_fps, c.wall_fps, c.cap_hz, 20.0);
        EXPECT(jobs >= c.expect - c.expect / 50 && jobs <= c.expect + c.expect / 50, "%s: %ld jobs in 20 game seconds, expected about %ld", c.what, jobs, c.expect);
    }

    // the schedule: a clock that steps back (a take ends, the fixed step is switched on or off) runs the job at once and counts on from there; a changed
    // layout always runs it
    const int64_t interval = 1'000'000'000ll / 30;
    race_line::RateSchedule s;
    s.Start(5'000'000'000'000ll, interval);
    EXPECT(!s.Due(true, 5'000'000'000'000ll + interval / 2, interval), "not due half an interval after the first job");
    EXPECT(s.Due(true, 5'000'000'000'000ll + interval, interval), "due one interval after the first job");
    EXPECT(s.Due(true, 3'000'000'000ll, interval), "the clock stepped back: the job runs");
    EXPECT(!s.Due(true, 3'000'000'000ll + interval / 2, interval), "after the step back it counts on from there (not due)");
    EXPECT(s.Due(true, 3'000'000'000ll + interval, interval), "after the step back it counts on from there (due)");
    EXPECT(s.Due(false, 3'000'000'000ll + interval + 1, interval), "a changed layout runs the job at once");
    EXPECT(!s.Due(true, 3'000'000'000ll + interval + 2, interval), "and the next job is an interval later");
}

// ---------------------------------------------------------------------------------------------- random cases

// Plausible values: most positive and of the size of speeds, tuning factors and times; some negative or zero; one scale per case.
struct Gen {
    Rng& rng;
    double scale;
    float Val() {
        const uint32_t r = rng.U(100);
        double v;
        if (r < 80) v = rng.D(0.05, 60.0);
        else if (r < 90) v = rng.D(-5.0, 5.0);
        else if (r < 95) v = 0.0;
        else v = rng.D(60.0, 400.0);
        return static_cast<float>(v * scale);
    }
};

// Fills the tuning area of the planner block, the vehicle block and the path with random numbers (the integers of the path are valid).
void FillCase(Arena& a, Rng& rng) {
    // 0.1, 1, 10 or 100 times the plausible values; one case in 25 at 1e-19, where products and squares are float denormals
    Gen g{rng, rng.U(25) == 0 ? 1e-19 : std::pow(10.0, static_cast<double>(rng.U(4)) - 1.0)};
    for (uint32_t off = 0; off < kCtxLen; off += 4) PutF(a, kCtx + off, 0.0f);
    for (uint32_t off = 25200; off < kCtxLen; off += 4) PutF(a, kCtx + off, g.Val());
    for (uint32_t off = 0; off < kVehLen; off += 4) PutF(a, kVeh + off, g.Val());
    // the divisor and the table's scale must not be near zero
    PutF(a, kVeh + 8, static_cast<float>(rng.D(0.2, 5.0)));
    PutF(a, kVeh + 44, static_cast<float>(rng.D(0.3, 3.0)));
    for (uint32_t off = 0; off < kPathLen; off += 4) PutF(a, kPath + off, g.Val());
    const uint32_t begin = rng.U(4);
    uint32_t count = rng.R(2, 62);
    if (rng.U(20) == 0) count = rng.R(0, 2);
    St32(a.base, kPath + kPathBegin, begin);
    St32(a.base, kPath + kPathEnd, begin + count);
    St32(a.base, kPath + kPathWork, rng.U(1000));
    // the first sample's field times veh + 44 is the table index: reach the whole table
    PutF(a, kPath + kSampleBase + begin * kSampleSize, static_cast<float>(rng.D(0.0, 120.0)));
    for (uint32_t off = 0; off < 256; off += 4) St32(a.base, kStackSp + off, 0xA5A5A500u + off);  // the caller's frame above r1
}

// A pass's tuning inputs plus its own state for the benchmark: a few hundred filled cases.
void BuildArena(Arena& a) {
    a.Commit(kWorkLo, kWorkHi);
    SeedConstants(a);
}

#ifdef RL_WITH_ORIGINALS
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
#define REX_MUSTTAIL
static uint64_t g_timebase = 1000;
#define REX_QUERY_TIMEBASE() (g_timebase += 977)  // the elapsed-time word at path + 5360 is left out of the comparison

using rex_gpr_member_ = PPCRegister PPCContext::*;
static constexpr rex_gpr_member_ rex_gpr_members_[18] = {&PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
                                                         &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24, &PPCContext::r25,
                                                         &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
static constexpr rex_gpr_member_ rex_fpr_members_[18] = {&PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18, &PPCContext::f19,
                                                         &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23, &PPCContext::f24, &PPCContext::f25,
                                                         &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29, &PPCContext::f30, &PPCContext::f31};
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
#define RL_FALLBACKS(N)                                                                  \
    extern "C" void __savegprlr_##N(PPCContext&, u8*) { std::abort(); }                  \
    extern "C" void __restgprlr_##N(PPCContext&, u8*) { std::abort(); }
RL_FALLBACKS(14) RL_FALLBACKS(15) RL_FALLBACKS(16) RL_FALLBACKS(17) RL_FALLBACKS(18) RL_FALLBACKS(19) RL_FALLBACKS(20) RL_FALLBACKS(21) RL_FALLBACKS(22)
RL_FALLBACKS(23) RL_FALLBACKS(24) RL_FALLBACKS(25) RL_FALLBACKS(26) RL_FALLBACKS(27) RL_FALLBACKS(28) RL_FALLBACKS(29) RL_FALLBACKS(30) RL_FALLBACKS(31)
// the console's __savefpr_N / __restfpr_N: stfd f<N>..f31 at r12 - 8 * (32 - n) (r12 points just above the saved block)
#define RL_FPR_HELPERS(N)                                                                                                    \
    extern "C" void __savefpr_##N(PPCContext& ctx, u8* base) {                                                               \
        for (unsigned n = N; n <= 31; ++n) REX_STORE_U64(ctx.r12.u32 - 8u * (32u - n), (ctx.*rex_fpr_members_[n - 14]).u64); \
    }                                                                                                                        \
    extern "C" void __restfpr_##N(PPCContext& ctx, u8* base) {                                                               \
        for (unsigned n = N; n <= 31; ++n) (ctx.*rex_fpr_members_[n - 14]).u64 = REX_LOAD_U64(ctx.r12.u32 - 8u * (32u - n)); \
    }
RL_FPR_HELPERS(14) RL_FPR_HELPERS(28)

#include "rl_originals.inc"

// The native passes as the game's overrides call them (hooks/race_line.cpp), for the "twin" copy of the job.
extern "C" void twin_hook_8216CBC0(PPCContext& ctx, u8* base) {
    ctx.fpscr.DisableFlushMode();
    ctx.f1.f64 = race_line::BackwardPass(base, race_line::Constants::Load(base), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
extern "C" void twin_hook_8216CDD0(PPCContext& ctx, u8* base) {
    ctx.fpscr.DisableFlushMode();
    race_line::ForwardPass(base, race_line::Constants::Load(base), ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.f1.f64);
}

namespace {

bool SameKeptRegs(const PPCContext& x, const PPCContext& y) {
    if (x.r1.u64 != y.r1.u64) return false;
    for (unsigned n = 14; n <= 31; ++n) {
        if ((x.*rex_gpr_members_[n - 14]).u64 != (y.*rex_gpr_members_[n - 14]).u64) return false;
        if ((x.*rex_fpr_members_[n - 14]).u64 != (y.*rex_fpr_members_[n - 14]).u64) return false;
    }
    return true;
}

PPCContext MakeCtx(Rng& rng) {
    PPCContext c{};
    c.r1.u64 = kStackSp;
    for (unsigned n = 14; n <= 31; ++n) {
        (c.*rex_gpr_members_[n - 14]).u64 = rng.Next();  // sentinels that must survive
        (c.*rex_fpr_members_[n - 14]).u64 = rng.Next();
    }
    c.lr = 0x82001234;
    c.r12.u64 = 0x82001234;
    c.fpscr.csr = _mm_getcsr();  // the host's control word (exceptions masked), as the game's contexts start
    c.vscr.nj = 1;               // vector code runs with flush-to-zero; the scalar passes switch it off and the vector code switches it on again
    c.r3.u64 = kCtx;
    c.r4.u64 = kVeh;
    c.r5.u64 = kPath;
    return c;
}

enum class Kind { kBackward, kForward, kChain };
const char* Name(Kind k) { return k == Kind::kBackward ? "backward" : k == Kind::kForward ? "forward" : "chain"; }

long g_nan_results = 0;
long g_nan_words = 0;  // path words that are NaN in both runs (accepted as equal)

// One case from the initial state `init`: the recompiled function(s) on `ao`, the native one(s) on `an`. `mutate` replaces the native
// constants by wrong ones (the comparison must then fail).
bool OneCase(Arena& ao, Arena& an, const Arena& init, Rng& rng, Kind kind, uint64_t id, bool mutate = false) {
    ao.CopyFrom(init);
    an.CopyFrom(init);
    const PPCContext in = MakeCtx(rng);
    PPCContext co = in;
    const double f1in = (kind == Kind::kForward) ? static_cast<double>(static_cast<float>(rng.D(0.0, 40.0))) : 0.0;
    co.f1.f64 = f1in;
    co.fpscr.DisableFlushMode();
    Constants k = Constants::Load(an.base);
    if (mutate) k.f[2] += 0.25;  // the 2.0f of the backward pass
    double f1n = f1in;
    if (kind == Kind::kBackward || kind == Kind::kChain) {
        orig_8216CBC0(co, ao.base);
        f1n = BackwardPass(an.base, k, kCtx, kVeh, kPath);
    }
    if (kind == Kind::kForward || kind == Kind::kChain) {
        orig_8216CDD0(co, ao.base);
        ForwardPass(an.base, k, kStackSp, kCtx, kVeh, kPath, f1n);
    }
    uint32_t where = 0;
    bool ok = ao.SameAs(an, &where, nullptr, 0, kPath, kPath + kPathLen, &g_nan_words);
    if (!ok && !mutate) EXPECT(false, "%s case %llu: memory differs at %#x (recompiled %02x, native %02x)", Name(kind), static_cast<unsigned long long>(id), where, ao.base[where], an.base[where]);
    if (kind == Kind::kBackward) {
        if (std::isnan(co.f1.f64)) ++g_nan_results;
        const bool same = co.f1.u64 == std::bit_cast<uint64_t>(f1n);
        if (!same && !mutate) EXPECT(false, "backward case %llu: f1 %016llx vs %016llx", static_cast<unsigned long long>(id), static_cast<unsigned long long>(co.f1.u64), static_cast<unsigned long long>(std::bit_cast<uint64_t>(f1n)));
        ok = ok && same;
    }
    if (!mutate) EXPECT(SameKeptRegs(co, in), "%s case %llu: the recompiled function changed a kept register (the test's save / restore)", Name(kind), static_cast<unsigned long long>(id));
    return ok;
}

void TestAgainstOriginals(const Arena& base_arena, long cases) {
    Arena ao, an;
    Rng rng(20261122);
    Arena init;
    init.CopyFrom(base_arena);
    long good = 0;
    for (const Kind kind : {Kind::kBackward, Kind::kForward, Kind::kChain}) {
        long ok = 0;
        for (long i = 0; i < cases; ++i) {
            FillCase(init, rng);
            ok += OneCase(ao, an, init, rng, kind, static_cast<uint64_t>(i)) ? 1 : 0;
        }
        std::printf("  %-8s %ld of %ld cases identical\n", Name(kind), ok, cases);
        good += ok;
    }
    // the comparison must have teeth: a wrong constant in a native pass is caught in most cases
    for (const Kind kind : {Kind::kBackward, Kind::kForward}) {
        long caught = 0;
        const long mut_cases = 2000;
        for (long i = 0; i < mut_cases; ++i) {
            FillCase(init, rng);
            if (!OneCase(ao, an, init, rng, kind, static_cast<uint64_t>(i), true)) ++caught;
        }
        std::printf("  mutation (a wrong 2.0f in the %s pass): caught in %ld of %ld cases\n", Name(kind), caught, mut_cases);
        EXPECT(caught > mut_cases / 3, "the mutation was caught in only %ld of %ld cases: the comparison is too weak", caught, mut_cases);
    }
    std::printf("  path words that were NaN in both runs (invalid operations, accepted as equal): %ld in %ld cases\n", g_nan_words, 3 * cases);
    EXPECT(g_nan_words < 3 * cases, "%ld NaN words: the generator is producing too many invalid inputs", g_nan_words);
    (void)good;
}

// ---------------------------------------------------------------------------------------------- benchmark

double TimeNs(const char* what, int rounds, void (*fn)(void*), void* arg, long calls_per_round) {
    using Clock = std::chrono::steady_clock;
    fn(arg);  // warm
    double best = 1e30;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = Clock::now();
        fn(arg);
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / static_cast<double>(calls_per_round);
        best = std::min(best, ns);
    }
    std::printf("  %-34s %9.1f ns per call\n", what, best);
    return best;
}

struct BenchSet {
    Arena a;                  // one arena, `n` cases written one after another is not possible (fixed addresses): a pristine copy per case
    std::vector<std::vector<uint8_t>> path, ctxb, vehb;  // saved blocks
    int n = 0;
    PPCContext ctx{};
    int pass = 0;  // 0 = backward recompiled, 1 = backward native, 2 = forward recompiled, 3 = forward native, 4 = restore only
};

void RunBench(void* p) {
    BenchSet& s = *static_cast<BenchSet*>(p);
    const Constants k = Constants::Load(s.a.base);
    for (int i = 0; i < s.n; ++i) {
        std::memcpy(s.a.base + kPath, s.path[i].data(), kPathLen);
        std::memcpy(s.a.base + kCtx + 25200, s.ctxb[i].data(), kCtxLen - 25200);
        std::memcpy(s.a.base + kVeh, s.vehb[i].data(), 1024);
        PPCContext c = s.ctx;
        c.fpscr.DisableFlushMode();
        switch (s.pass) {
            case 0: orig_8216CBC0(c, s.a.base); break;
            case 1: c.f1.f64 = BackwardPass(s.a.base, k, kCtx, kVeh, kPath); break;
            case 2: c.f1.f64 = 12.5; orig_8216CDD0(c, s.a.base); break;
            case 3: ForwardPass(s.a.base, k, kStackSp, kCtx, kVeh, kPath, 12.5); break;
            default: break;
        }
    }
}

void Bench(const Arena& base_arena) {
    BenchSet s;
    s.a.CopyFrom(base_arena);
    Rng rng(77);
    s.n = 256;
    for (int i = 0; i < s.n; ++i) {
        FillCase(s.a, rng);
        // a typical path: 40 to 62 samples
        St32(s.a.base, kPath + kPathBegin, 0);
        St32(s.a.base, kPath + kPathEnd, rng.R(40, 62));
        s.path.emplace_back(s.a.base + kPath, s.a.base + kPath + kPathLen);
        s.ctxb.emplace_back(s.a.base + kCtx + 25200, s.a.base + kCtx + kCtxLen);
        s.vehb.emplace_back(s.a.base + kVeh, s.a.base + kVeh + 1024);
    }
    s.ctx = MakeCtx(rng);
    std::printf("bench (%d paths, best of 20 rounds):\n", s.n);
    s.pass = 4;
    const double base = TimeNs("restore the inputs only", 20, RunBench, &s, s.n);
    s.pass = 0;
    const double b0 = TimeNs("backward pass, recompiled", 20, RunBench, &s, s.n) - base;
    s.pass = 1;
    const double b1 = TimeNs("backward pass, native", 20, RunBench, &s, s.n) - base;
    s.pass = 2;
    const double f0 = TimeNs("forward pass, recompiled", 20, RunBench, &s, s.n) - base;
    s.pass = 3;
    const double f1 = TimeNs("forward pass, native", 20, RunBench, &s, s.n) - base;
    std::printf("  speed-up (restore time subtracted): backward %.2fx, forward %.2fx\n", b0 / std::max(b1, 1.0), f0 / std::max(f1, 1.0));
}

// ---------------------------------------------------------------------------------------------- (3) replay of recorded jobs

#endif  // RL_WITH_ORIGINALS

struct Record {
    uint32_t r3, r4, r5, r6, len3, len4, len5, len6, index, job;
    std::vector<uint8_t> b3, b4, b5, b6;
};

bool ReadRecords(const char* path, std::vector<Record>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("cannot open %s\n", path);
        return false;
    }
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "RLSNAP01", 8) != 0) {
        std::printf("%s: not a Race Line recording (no RLSNAP01)\n", path);
        std::fclose(f);
        return false;
    }
    for (;;) {
        uint8_t h[64];
        if (std::fread(h, 1, 64, f) != 64) break;
        auto get32 = [&](size_t at) { uint32_t v; std::memcpy(&v, h + at, 4); return v; };
        if (get32(0) != 0x314A4C52u || get32(4) != 64) {
            std::printf("%s: bad record header after %zu records\n", path, out.size());
            std::fclose(f);
            return false;
        }
        Record r{};
        r.r3 = get32(8);  r.r4 = get32(12);  r.r5 = get32(16);  r.r6 = get32(20);
        r.len3 = get32(24); r.len4 = get32(28); r.len5 = get32(32); r.len6 = get32(36);
        r.index = get32(48); r.job = get32(52);
        auto rd = [&](std::vector<uint8_t>& b, uint32_t n) {
            b.resize(n);
            return std::fread(b.data(), 1, n, f) == n;
        };
        if (!rd(r.b3, r.len3) || !rd(r.b4, r.len4) || !rd(r.b5, r.len5) || !rd(r.b6, r.len6)) {
            std::printf("%s: truncated record %zu\n", path, out.size());
            break;
        }
        out.push_back(std::move(r));
    }
    std::fclose(f);
    return true;
}

void ParseSummary(const std::vector<Record>& recs) {
    std::printf("%zu records\n", recs.size());
    for (size_t i = 0; i < recs.size() && i < 12; ++i) {
        const Record& r = recs[i];
        auto w = [&](uint32_t off) { uint32_t v; std::memcpy(&v, r.b5.data() + off, 4); return __builtin_bswap32(v); };
        std::printf("  #%u job %u: ctx %08x veh %08x path %08x scratch %08x, samples %u..%u, ring %u/%u/%u, laid %u\n", r.index, r.job, r.r3, r.r4, r.r5, r.r6, w(kPathBegin), w(kPathEnd), w(1028),
                    w(1032), w(1040), w(5344));
    }
}

#ifdef RL_WITH_ORIGINALS
void Replay(const Arena& base_arena, const std::vector<Record>& recs) {
    Arena ao, an;
    long same = 0, differ = 0;
    for (const Record& r : recs) {
        Arena init;
        init.CopyFrom(base_arena);
        init.Commit(r.r3, r.r3 + r.len3);
        init.Commit(r.r4, r.r4 + r.len4);
        init.Commit(r.r5, r.r5 + r.len5);
        init.Commit(r.r6, r.r6 + r.len6);
        init.Commit(kWorkLo, kWorkHi);
        std::memcpy(init.base + r.r3, r.b3.data(), r.len3);
        std::memcpy(init.base + r.r4, r.b4.data(), r.len4);
        std::memcpy(init.base + r.r5, r.b5.data(), r.len5);
        std::memcpy(init.base + r.r6, r.b6.data(), r.len6);
        ao.CopyFrom(init);
        an.CopyFrom(init);
        PPCContext co{};
        co.r1.u64 = kStackSp;
        co.r3.u64 = r.r3;
        co.r4.u64 = r.r4;
        co.r5.u64 = r.r5;
        co.r6.u64 = r.r6;
        co.lr = 0x82001234;
        co.fpscr.csr = _mm_getcsr();
        co.vscr.nj = 1;
        PPCContext cn = co;
        co.fpscr.DisableFlushMode();
        cn.fpscr.DisableFlushMode();
        orig_82171078(co, ao.base);
        twin_82171078(cn, an.base);
        uint32_t where = 0;
        const uint32_t skip[1] = {r.r5 + kElapsedWord};
        if (ao.SameAs(an, &where, skip, 1)) {
            ++same;
        } else {
            ++differ;
            const char* region = where >= r.r5 && where < r.r5 + r.len5 ? "path" : where >= r.r6 && where < r.r6 + r.len6 ? "scratch" : where >= r.r3 && where < r.r3 + r.len3 ? "tuning block" : "other";
            EXPECT(false, "record %u: memory differs at %#x (%s + %u): recompiled %02x, native %02x", r.index, where, region,
                   where - (region[0] == 'p' ? r.r5 : region[0] == 's' ? r.r6 : r.r3), ao.base[where], an.base[where]);
        }
    }
    std::printf("replay: %ld of %zu recorded jobs come out identical, %ld differ\n", same, recs.size(), differ);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    AddVectoredExceptionHandler(1, GuestFault);
#endif
    const char* image = nullptr;
    const char* replay = nullptr;
    bool bench = false, parse_only = false;
    long cases = 6000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--replay" && i + 1 < argc) replay = argv[++i];
        else if (a == "--replay-parse" && i + 1 < argc) { replay = argv[++i]; parse_only = true; }
        else if (a == "--bench") bench = true;
        else if (a == "--cases" && i + 1 < argc) cases = std::atol(argv[++i]);
    }
    TestHelpers(image);
    TestRateCap();
    if (replay) {
        std::vector<Record> recs;
        if (!ReadRecords(replay, recs)) return 2;
        ParseSummary(recs);
        if (parse_only) return 0;
#ifdef RL_WITH_ORIGINALS
        if (!image) {
            std::printf("--replay needs --image <image.bin> (the job reads constants from the game's image)\n");
            return 2;
        }
        Arena base_arena;
        base_arena.Commit(kWorkLo, kWorkHi);
        if (!LoadImage(base_arena, image)) {
            std::printf("cannot read the image %s\n", image);
            return 2;
        }
        Replay(base_arena, recs);
#else
        std::printf("--replay needs a build with -DRL_WITH_ORIGINALS\n");
        return 2;
#endif
    }
#ifdef RL_WITH_ORIGINALS
    if (!replay && Fma3Available()) {
        Arena base_arena;
        BuildArena(base_arena);  // the constants are seeded; --image only checks them (TestHelpers): a 10 MB image per case would make the copies slow
        std::printf("random cases against the recompiled functions:\n");
        TestAgainstOriginals(base_arena, cases);
        if (bench) Bench(base_arena);
    }
#else
    (void)bench;
    (void)cases;
    std::printf("note: built without -DRL_WITH_ORIGINALS: only the reference checks ran\n");
#endif
    std::printf("race_line_test: %ld checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
