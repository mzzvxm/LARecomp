// The AI racers' driving line ("UpdateRaceLine" job, pool "[RAGE] Race Line Task"): a rate cap, native
// versions of its two hottest functions, and a debug recorder. perf\RACE-LINE.md has the profile and the
// reading of the code; race_line_native.h says what the functions are.
//
// What runs, per AI vehicle and per frame (sub_822BE670, the AI brain): when the planner's control points
// are not all laid out yet, sub_8216C7E0 copies a job descriptor into a slot of the job ring and the pool's
// two threads run sub_8216C7C8 -> sub_82171078, which improves the line (up to 100 trial moves of one control
// point, each a full pass over the path's samples). At the next frame the brain joins the job
// (sub_821C4458, only when the handle at brain + 304 is not 0) before it touches the planner again. On the
// console the game ran at 30 FPS, so a planner was improved up to 30 times a second; at 143 FPS two jobs
// of about 2.5 ms run every frame, 20% of all the CPU of a race (guest self time of the AB011 race runs).
//
// 1. The rate cap (race_line_max_hz, default 30 = the console's rate, 0 = every frame as before): the
//    queue function and its synchronous twin (sub_8254DEA0) are overridden. A call is let through when the
//    planner's last job is older than 1 / hz, or when the planner's layout (the ring's counters and the number of
//    control points laid out, which only the planner's own update changes) is not the one the last job saw; otherwise
//    nothing is queued and the brain's handle stays 0 (it joins nothing, as when no job ran). The skipped
//    frames keep the previous result: the numbers of the samples (speed profile) are up to 1 / hz old, the
//    samples themselves are never out of step with the ring, because a layout change always runs the job.
//    The cap is a change in what the game computes, not an exact replacement: an A/B check is in RACE-LINE.md.
//    The cap counts the game's own time (race_line_cap.h CapClockNs): the steady clock, but under a fixed step (a take, or the dev setting
//    mcla_fixed_step_fps alone) the updates run times the step, so a take at any rate (fixed step, lock-step with the readback) or a fixed-step
//    run at any wall FPS gets 30 jobs per second of game time, not of wall time. perf\scripts\chain-raceline-ai.ps1 (the AI gate) runs the
//    races as takes for that reason.
// 2. Native versions (race_line_native_mask, default 3, read at start): bit 0 the backward speed pass
//    (sub_8216CBC0), bit 1 the forward pass (sub_8216CDD0). Bit for bit the recompiled functions' results
//    (race_line_test.cpp); they need FMA3 and plain guest RAM, and otherwise the recompiled function runs.
//    The first race_line_check_calls (3000) calls of each pass run both versions on the same input in the game and
//    compare the path they leave (and f1); a difference switches that pass back to the recompiled function for the
//    session and says where in the log (the same safety as lzx_native's).
// 3. Debug (race_line_stats, race_line_record): one log line per 10 s with the jobs queued, skipped and run and the
//    time they took; and a file of job inputs (race_line_record) for race_line_test.cpp's replay.
//
// What the overrides leave in the registers: the natives touch no register but f1 (the backward pass's result,
// the forward pass's argument); the volatile registers keep the caller's values instead of the function's scratch
// values (dead after a call by the ABI; the call sites were scanned in perf\RACE-LINE.md). A skipped queue call
// returns r3 = 0, the value a full ring returns.

#if !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../cinematic/capture_mode.h"
#include "gpu/live_text.h"
#include "larecomp_log.h"
#include "race_line_cap.h"
#include "race_line_native.h"
#include "settings/larecomp_settings.h"

REX_EXTERN(__imp__rex_sub_8216C7E0);  // queues the job: r3 = the planner; returns the job's handle in r3 (0 = none)
REX_EXTERN(__imp__rex_sub_8254DEA0);  // runs the job in place (the game's single-thread mode): r3 = the planner
REX_EXTERN(__imp__rex_sub_82171078);  // the job: r3 = tuning block (planner + 16), r4 = vehicle tuning, r5 = path (planner + 25440), r6 = scratch
REX_EXTERN(__imp__rex_sub_8216CBC0);
REX_EXTERN(__imp__rex_sub_8216CDD0);

namespace {

using Clock = std::chrono::steady_clock;

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

// The clock of the rate cap: the game's own time (race_line_cap.h CapClockNs). The steady clock, except under a fixed step (a take, or mcla_fixed_step_fps
// alone), where every update is exactly the step long and the clock is the updates run times it. So "30 jobs a second" is 30 a second of GAME time in a take
// or a fixed-step run (a take at 144 FPS that takes 9 ms of wall time a frame, one at 30 FPS that takes 60 ms, or a fixed step of 1/30 s run at 240 wall FPS
// still gives 30 per game second), and the same as before everywhere else (the steady clock, and a cvar read for the step).
// The stats and the recorder keep the plain steady clock (they measure wall time).
int64_t CapNowNs() { return race_line::CapClockNs(mc::cinematic::FixedStepTicks(), mc::cinematic::UpdateCount(), NowNs()); }

// ---------------------------------------------------------------------------------------------- stats

std::atomic<uint64_t> g_queued{0}, g_skipped_time{0}, g_run{0}, g_run_ns{0}, g_run_max_ns{0};
// mirrors of the native passes' counters for the report line (set where the counters live, below)
std::atomic<uint32_t> g_checked_report[2];
std::atomic<uint64_t> g_declined_report{0};
std::atomic<bool> g_off_report[2];
std::atomic<int64_t> g_last_report_ns{0};

std::string TakePlannerSummary();  // below, with the planner table: "addr q/s ..." of this window (and resets the window), empty when none ran

void MaybeReport() {
    const int64_t now = NowNs();
    int64_t last = g_last_report_ns.load(std::memory_order_relaxed);
    if (last == 0) {
        g_last_report_ns.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 10'000'000'000ll) return;
    if (!g_last_report_ns.compare_exchange_strong(last, now)) return;
    const uint64_t q = g_queued.exchange(0), s = g_skipped_time.exchange(0), r = g_run.exchange(0), ns = g_run_ns.exchange(0), mx = g_run_max_ns.exchange(0);
    const double secs = static_cast<double>(now - last) / 1e9;
    LARECOMP_APP_INFO("[RaceLine] last {:.1f} s: {} job(s) queued ({:.0f}/s), {} skipped by the cap, {} run ({:.2f} ms each, longest {:.1f} ms, {:.0f} ms of CPU per second); native passes: {} / {} calls "
                      "checked, {} declined (not plain RAM), off by the check: {} / {}",
                      secs, q, static_cast<double>(q) / secs, s, r, r ? static_cast<double>(ns) / static_cast<double>(r) / 1e6 : 0.0, static_cast<double>(mx) / 1e6,
                      static_cast<double>(ns) / 1e6 / secs, g_checked_report[0].load(), g_checked_report[1].load(), g_declined_report.load(), g_off_report[0].load() ? "backward" : "-",
                      g_off_report[1].load() ? "forward" : "-");
    // Per AI racer (a planner is one racer's brain): the same window's queue calls it let through (q) and skipped (s). The AI gate (perf\scripts\chain-raceline-ai.ps1) reads
    // how many racers have a planner and how their jobs spread.
    const std::string planners = TakePlannerSummary();
    if (!planners.empty()) LARECOMP_APP_INFO("[RaceLine] planners (window {:.1f} s, guest address queued/skipped): {}", secs, planners);
}

// ---------------------------------------------------------------------------------------------- the rate cap

// The planner's layout words (planner + 25440 is the path; + 1028 .. + 1040 are the ring's counters, + 5344 the number of
// control points laid out): changed only by the planner's own update (sub_8254D570), never by the job. (The sample range at
// +5024 / +5028 is written by the job, sub_8216E408, so it is not part of the layout.)
constexpr uint32_t kPathOffset = 25440;
constexpr uint32_t kLayoutOffsets[] = {kPathOffset + 1028, kPathOffset + 1032, kPathOffset + 1040, kPathOffset + 5344};
constexpr size_t kLayoutWords = sizeof(kLayoutOffsets) / sizeof(kLayoutOffsets[0]);
constexpr size_t kMaxPlanners = 64;

struct Planner {
    uint32_t key = 0;
    bool valid = false;
    uint32_t layout[kLayoutWords] = {};
    race_line::RateSchedule due;  // the planner's schedule on the cap's clock
    uint64_t queued = 0;       // this stats window's queue calls let through / skipped (race_line_stats; the report line names them per planner)
    uint64_t skipped = 0;
    uint64_t used = 0;         // g_planner_use at the planner's last queue call: the slot with the smallest one is the one a new planner takes when the table is full
};

std::mutex g_planner_mutex;
Planner g_planners[kMaxPlanners];
uint64_t g_planner_use = 0;  // counts the queue calls that reach the table (g_planner_mutex held)

Planner* FindPlanner(uint32_t planner, Planner** free_slot) {  // g_planner_mutex held
    for (Planner& p : g_planners) {
        if (p.valid && p.key == planner) return &p;
        if (!p.valid && free_slot && !*free_slot) *free_slot = &p;
    }
    return nullptr;
}

// The stats window's per-planner counts (see MaybeReport); resets them.
std::string TakePlannerSummary() {
    std::string out;
    std::lock_guard<std::mutex> lock(g_planner_mutex);
    for (Planner& p : g_planners) {
        if (!p.valid || (p.queued == 0 && p.skipped == 0)) continue;
        char text[48];
        std::snprintf(text, sizeof text, "%s%08x %llu/%llu", out.empty() ? "" : ", ", p.key, static_cast<unsigned long long>(p.queued), static_cast<unsigned long long>(p.skipped));
        out += text;
        p.queued = 0;
        p.skipped = 0;
    }
    return out;
}

// With no cap (every frame) the queue call is let through as before; the planner's count is noted for the report only.
void CountCall(uint32_t planner, bool queued) {
    Planner* free_slot = nullptr;
    std::lock_guard<std::mutex> lock(g_planner_mutex);
    Planner* slot = FindPlanner(planner, &free_slot);
    if (!slot) {
        if (!free_slot) return;  // table full: not counted
        slot = free_slot;
        *slot = Planner{};
        slot->key = planner;
        slot->valid = true;
    }
    (queued ? slot->queued : slot->skipped) += 1;
}

// True when a job for this planner should run now (and notes it). cap_hz > 0. `count`: keep the report's per-planner counts (race_line_stats).
bool JobDue(const uint8_t* base, uint32_t planner, int cap_hz, bool count) {
    if (!race_line::PlainBlock(planner, kPathOffset + 5376)) return true;  // not a planner we understand: never skip
    uint32_t layout[kLayoutWords];
    for (size_t i = 0; i < kLayoutWords; ++i) layout[i] = race_line::Ld32(base, planner + kLayoutOffsets[i]);
    const int64_t now = CapNowNs();
    const int64_t interval = 1'000'000'000ll / cap_hz;
    std::lock_guard<std::mutex> lock(g_planner_mutex);
    Planner* free_slot = nullptr;
    Planner* slot = FindPlanner(planner, &free_slot);
    if (!slot) {
        // Table full (the planners of earlier races stay in it; a racer's address is the key): the planner that asked longest ago goes. (Not `planner % kMaxPlanners`: the
        // guest's allocations are aligned, so that reaches few of the slots, live racers evict each other, and every call of an evicted one is a first call, which is let
        // through: the cap would be gone for those racers for the rest of the session.)
        if (!free_slot) {
            free_slot = &g_planners[0];
            for (Planner& p : g_planners) {
                if (p.used < free_slot->used) free_slot = &p;
            }
        }
        slot = free_slot;
        *slot = Planner{};
        slot->key = planner;
        slot->valid = true;
        std::memcpy(slot->layout, layout, sizeof(layout));
        slot->due.Start(now, interval);
        slot->used = ++g_planner_use;
        if (count) slot->queued += 1;
        return true;
    }
    slot->used = ++g_planner_use;
    const bool same_layout = std::memcmp(slot->layout, layout, sizeof(layout)) == 0;
    if (!slot->due.Due(same_layout, now, interval)) {  // (a clock that stepped back starts the planner's schedule again)
        if (count) slot->skipped += 1;
        return false;
    }
    std::memcpy(slot->layout, layout, sizeof(layout));
    if (count) slot->queued += 1;
    return true;
}

bool Skip(const uint8_t* base, uint32_t planner) {
    const int cap = larecomp::settings::race_line_max_hz.Get();
    const bool stats = larecomp::settings::race_line_stats.Get();
    if (cap <= 0) {
        if (stats) CountCall(planner, true);
        return false;
    }
    static std::atomic<int> said_cap{-1};
    if (said_cap.exchange(cap, std::memory_order_relaxed) != cap) {
        LARECOMP_APP_INFO("[RaceLine] the rate cap: {} jobs a second per racer, counted on the scene clock (game time: the steady clock, or under a fixed step the updates run x the step)", cap);
    }
    if (JobDue(base, planner, cap, stats)) return false;
    g_skipped_time.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------------------------- native passes

enum Pass : uint32_t { kBackward = 0, kForward = 1, kPassCount = 2 };
const char* const kPassNames[kPassCount] = {"backward speed pass (sub_8216CBC0)", "forward pass (sub_8216CDD0)"};

std::atomic<bool> g_pass_off[kPassCount];      // switched off by the check
std::atomic<uint32_t> g_checked[kPassCount];   // calls checked so far

uint32_t NativeMask() {
    static const uint32_t mask = [] {
        uint32_t m = static_cast<uint32_t>(larecomp::settings::race_line_native_mask.Get()) & 3u;
#if RL_HAS_X86
        const bool fma = race_line::Fma3Available();
#else
        const bool fma = false;
#endif
        if (m && !fma) {
            LARECOMP_APP_INFO("[RaceLine] this CPU has no FMA3 (or the OS does not save AVX state): the Race Line passes stay recompiled");
            m = 0;
        }
        LARECOMP_APP_INFO("[RaceLine] native passes: mask {:#x} (bit 0 backward speed pass sub_8216CBC0, bit 1 forward pass sub_8216CDD0; 0 = recompiled); the first {} calls of each are checked "
                          "against the recompiled function; rate cap {} Hz (0 = every frame)",
                          m, larecomp::settings::race_line_check_calls.Get(), larecomp::settings::race_line_max_hz.Get());
        return m;
    }();
    return mask;
}

uint32_t CheckLimit() {
    static const uint32_t limit = static_cast<uint32_t>(larecomp::settings::race_line_check_calls.Get());
    return limit;
}

bool PassOn(Pass p) { return ((NativeMask() >> p) & 1u) != 0 && !g_pass_off[p].load(std::memory_order_relaxed); }

#if RL_HAS_X86
const race_line::Constants& Consts(const uint8_t* base) {
    static const race_line::Constants k = race_line::Constants::Load(base);
    return k;
}
#endif

// The blocks a pass reads or writes, as plain guest RAM: the planner's tuning block (to + 25424), the vehicle's tuning block, the path.
// (The stack: every guest stack lies below the physical window, the recompiled code's own stack accesses assume it.)
bool PlainPassBlocks(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    const bool plain = r1 >= 0x10000u && r1 < race_line::kPhysicalWindow - 256 && race_line::PlainBlock(r3, 25424) && race_line::PlainBlock(r4, 8780) && race_line::PlainBlock(r5, 5376);
    if (!plain) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) LARECOMP_APP_WARN("[RaceLine] a Race Line pass got blocks that are not plain guest RAM (r1 {:#x}, r3 {:#x}, r4 {:#x}, r5 {:#x}): such calls stay recompiled", r1, r3, r4, r5);
    }
    return plain;
}

constexpr uint32_t kPathBytes = 5376;  // the one block the passes write

// The path, as it was before a checked call and as the native pass left it.
struct CheckBuffers {
    uint8_t before[kPathBytes];
    uint8_t native[kPathBytes];
};

// Equal as big-endian float words, or both NaN (an invalid operation's NaN depends on operand order; real data has none).
bool SameWords(const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t* first) {
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        if (std::memcmp(a + i, b + i, 4) == 0) continue;
        const float x = std::bit_cast<float>(race_line::Ld32(a, i)), y = std::bit_cast<float>(race_line::Ld32(b, i));
        if (std::isnan(x) && std::isnan(y)) continue;
        *first = i;
        return false;
    }
    return true;
}

// Judges one checked call: the native result in buf.native / native_f1 (taken before the recompiled function ran) against what the recompiled
// function left in guest memory and in f1.
void Verdict(Pass p, const uint8_t* base, uint32_t r5, const CheckBuffers& buf, double native_f1, double game_f1, bool check_f1, bool spill_same) {
    uint32_t first = 0;
    bool same = SameWords(buf.native, base + r5, kPathBytes, &first) && spill_same;
    if (check_f1) {
        const uint64_t a = std::bit_cast<uint64_t>(native_f1), b = std::bit_cast<uint64_t>(game_f1);
        same = same && (a == b || (std::isnan(native_f1) && std::isnan(game_f1)));
    }
    const uint32_t n = g_checked[p].fetch_add(1, std::memory_order_relaxed) + 1;
    g_checked_report[p].store(n, std::memory_order_relaxed);
    if (!same) {
        g_off_report[p].store(true, std::memory_order_relaxed);
        if (!g_pass_off[p].exchange(true)) {
            uint32_t was = 0, now = 0;
            std::memcpy(&was, buf.native + first, 4);
            std::memcpy(&now, base + r5 + first, 4);
            LARECOMP_APP_WARN("[RaceLine] the native {} differs from the recompiled one (call {}: path + {} native {:08x} recompiled {:08x}{}): the native pass is switched off for this session",
                              kPassNames[p], n, first, __builtin_bswap32(was), __builtin_bswap32(now), spill_same ? "" : ", or the spilled argument");
        }
    } else if (n == CheckLimit()) {
        LARECOMP_APP_INFO("[RaceLine] the native {}: the first {} calls came out bit for bit like the recompiled function's", kPassNames[p], n);
    }
}

// ---------------------------------------------------------------------------------------------- recorder

// File format (little-endian host fields; the guest blocks are copied byte for byte, so big-endian as in guest memory):
//   "RLSNAP01"                                   8 bytes
//   per record:
//     u32 tag 'RLJ1' (0x314A4C52)
//     u32 header_bytes (= 64)
//     u32 r3, r4, r5, r6                         the job's arguments (guest addresses)
//     u32 len3, len4, len5, len6                 bytes recorded at each (25424, 8780, 5376, 21600)
//     u64 qpc_ns                                 host steady clock, informational
//     u32 index                                  0-based record number
//     u32 job_number                             how many jobs the game had run when this one started
//     u8 padding to header_bytes
//     the four blocks in order r3, r4, r5, r6
constexpr uint32_t kLen3 = 25424, kLen4 = 8780, kLen5 = 5376, kLen6 = 21600;

// race_line_record is a string cvar that the menu's thread may write while the pool's threads run jobs, and a plain read of a string cvar can tear against
// that write (gpu/live_text.h says why): the jobs read it through a LiveText, which keeps the text in one atomic word set by the cvar's change callback.
mcla::gpu::LiveText& RecordPath() {
    static mcla::gpu::LiveText text("race_line_record");
    return text;
}

std::mutex g_record_mutex;
FILE* g_record_file = nullptr;
bool g_record_failed = false;
uint32_t g_recorded = 0;
std::atomic<uint64_t> g_job_number{0};

void Record(const uint8_t* base, const std::string& path, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint64_t job_number) {
    const int every = larecomp::settings::race_line_record_every.Get();
    const int limit = larecomp::settings::race_line_record_count.Get();
    if (path.empty() || limit <= 0) return;
    if (every > 1 && (job_number % static_cast<uint64_t>(every)) != 0) return;
    if (!race_line::PlainBlock(r3, kLen3) || !race_line::PlainBlock(r4, kLen4) || !race_line::PlainBlock(r5, kLen5) || !race_line::PlainBlock(r6, kLen6)) return;
    std::lock_guard<std::mutex> lock(g_record_mutex);
    if (g_record_failed || g_recorded >= static_cast<uint32_t>(limit)) return;
    if (!g_record_file) {
        g_record_file = std::fopen(path.c_str(), "wb");
        if (!g_record_file) {
            g_record_failed = true;
            LARECOMP_APP_WARN("[RaceLine] cannot open the recording file {}", path);
            return;
        }
        std::fwrite("RLSNAP01", 1, 8, g_record_file);
        LARECOMP_APP_INFO("[RaceLine] recording job inputs to {} (every {} job(s), up to {})", path, every, limit);
    }
    uint8_t header[64] = {};
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(0, 0x314A4C52u);
    put32(4, 64);
    put32(8, r3);
    put32(12, r4);
    put32(16, r5);
    put32(20, r6);
    put32(24, kLen3);
    put32(28, kLen4);
    put32(32, kLen5);
    put32(36, kLen6);
    const uint64_t ns = static_cast<uint64_t>(NowNs());
    std::memcpy(header + 40, &ns, 8);
    put32(48, g_recorded);
    put32(52, static_cast<uint32_t>(job_number));
    std::fwrite(header, 1, sizeof(header), g_record_file);
    std::fwrite(base + r3, 1, kLen3, g_record_file);
    std::fwrite(base + r4, 1, kLen4, g_record_file);
    std::fwrite(base + r5, 1, kLen5, g_record_file);
    std::fwrite(base + r6, 1, kLen6, g_record_file);
    std::fflush(g_record_file);
    if (++g_recorded >= static_cast<uint32_t>(limit)) {
        LARECOMP_APP_INFO("[RaceLine] recorded {} job inputs, done", g_recorded);
        std::fclose(g_record_file);
        g_record_file = nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------- overrides

extern "C" REX_FUNC(rex_sub_8216C7E0) {
    // r3 = the planner. Returns the job handle in r3; 0 = no job (what a full ring returns).
    if (Skip(base, ctx.r3.u32)) {
        ctx.r3.u64 = 0;
        return;
    }
    g_queued.fetch_add(1, std::memory_order_relaxed);
    __imp__rex_sub_8216C7E0(ctx, base);
}

extern "C" REX_FUNC(rex_sub_8254DEA0) {
    // The single-thread mode: the same call in place; r3 = the planner; nothing is returned.
    if (Skip(base, ctx.r3.u32)) return;
    __imp__rex_sub_8254DEA0(ctx, base);
}

extern "C" REX_FUNC(rex_sub_82171078) {
    // The job body (runs on the pool's threads): only the debug paths do anything here.
    const bool stats = larecomp::settings::race_line_stats.Get();
    const std::string record_path = RecordPath().Get();  // once for this job (a live change takes effect at the next job)
    const bool record = !record_path.empty();
    if (!stats && !record) {
        __imp__rex_sub_82171078(ctx, base);
        return;
    }
    const uint64_t number = g_job_number.fetch_add(1, std::memory_order_relaxed);
    if (record) Record(base, record_path, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, number);
    const int64_t t0 = NowNs();
    __imp__rex_sub_82171078(ctx, base);
    if (stats) {
        const uint64_t dt = static_cast<uint64_t>(NowNs() - t0);
        g_run.fetch_add(1, std::memory_order_relaxed);
        g_run_ns.fetch_add(dt, std::memory_order_relaxed);
        uint64_t prev = g_run_max_ns.load(std::memory_order_relaxed);
        while (dt > prev && !g_run_max_ns.compare_exchange_weak(prev, dt, std::memory_order_relaxed)) {
        }
        MaybeReport();
    }
}

extern "C" REX_FUNC(rex_sub_8216CBC0) {
    // r3 = tuning block, r4 = vehicle tuning, r5 = path; returns f1 (the mean of the samples' +36 fields).
#if RL_HAS_X86
    if (PassOn(kBackward)) {
        if (PlainPassBlocks(ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) {
            ctx.fpscr.DisableFlushMode();
            const race_line::Constants& k = Consts(base);
            if (CheckLimit() == 0 || g_checked[kBackward].load(std::memory_order_relaxed) >= CheckLimit()) {
                ctx.f1.f64 = race_line::BackwardPass(base, k, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
                return;
            }
            // A checked call: the native pass first, then the recompiled function on the same input; the game gets the recompiled result.
            static thread_local CheckBuffers buf;
            uint8_t* path = base + ctx.r5.u32;
            std::memcpy(buf.before, path, kPathBytes);
            const double native_f1 = race_line::BackwardPass(base, k, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
            std::memcpy(buf.native, path, kPathBytes);
            std::memcpy(path, buf.before, kPathBytes);
            __imp__rex_sub_8216CBC0(ctx, base);
            Verdict(kBackward, base, ctx.r5.u32, buf, native_f1, ctx.f1.f64, true, true);
            return;
        }
        g_declined_report.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    __imp__rex_sub_8216CBC0(ctx, base);
}

extern "C" REX_FUNC(rex_sub_8216CDD0) {
    // r3 = tuning block, r4 = vehicle tuning, r5 = path, f1 = the backward pass's result; returns nothing.
#if RL_HAS_X86
    if (PassOn(kForward)) {
        if (PlainPassBlocks(ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) {
            ctx.fpscr.DisableFlushMode();
            const race_line::Constants& k = Consts(base);
            if (CheckLimit() == 0 || g_checked[kForward].load(std::memory_order_relaxed) >= CheckLimit()) {
                race_line::ForwardPass(base, k, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.f1.f64);
                return;
            }
            static thread_local CheckBuffers buf;
            uint8_t* path = base + ctx.r5.u32;
            const uint32_t spill = ctx.r1.u32 + 44;  // the pass spills its argument to the caller's frame
            const uint32_t spill_before = race_line::Ld32(base, spill);
            std::memcpy(buf.before, path, kPathBytes);
            race_line::ForwardPass(base, k, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.f1.f64);
            std::memcpy(buf.native, path, kPathBytes);
            const uint32_t spill_native = race_line::Ld32(base, spill);
            std::memcpy(path, buf.before, kPathBytes);
            race_line::St32(base, spill, spill_before);
            __imp__rex_sub_8216CDD0(ctx, base);
            Verdict(kForward, base, ctx.r5.u32, buf, 0.0, 0.0, false, race_line::Ld32(base, spill) == spill_native);
            return;
        }
        g_declined_report.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    __imp__rex_sub_8216CDD0(ctx, base);
}
#endif  // !REXGLUE_HAS_XEO3_TARGET
