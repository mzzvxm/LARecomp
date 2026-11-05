// Frame-time instrumentation (MCLA_TIMING_LOG=1): per-second frame, GPU and
// guest counters plus a frame-time histogram in logs/timing_*.log, and the GPU
// interrupt probe it reports.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>

#include "hooks.h"
#include "hooks_internal.h"
#include "../draw_stats.h"

REXCVAR_DECLARE(int32_t, fps_limit);  // frame_timing.cpp

// ── Frame-time instrumentation ─────────────────────────────────────────
//
// Off unless MCLA_TIMING_LOG=1, so normal play pays one already-resolved bool
// test per frame. Ported from the midnightclub fork, which had it while this
// build did not - a performance collapse here previously left nothing in the
// log to diagnose it.
//
// Accumulates into counters and touches the filesystem at most once per
// second, never from inside a frame that is already late.

// 1 ms buckets; the last bucket is everything at or above it.
constexpr int kHistBuckets = 121;
constexpr double kHistogramWindowSec = 30.0;

bool TimingLogEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("MCLA_TIMING_LOG");
        return e && *e == '1';
    }();
    return enabled;
}

// Substep count, published by Patch_DeltaTime. The loop makes r24+1 passes.
std::atomic<int32_t> g_substep_last{-999};
// How often the loc_821BDB90 fixed-step path ran; published by MCLAFixedStepPath.
std::atomic<uint64_t> g_fixedstep_hits{0};

// Per-second accumulation of the runtime perf counters.
//
// The runtime resets these every frame, so a single read at the report
// boundary describes one arbitrary frame, not the second. SampleCounters()
// runs once per frame (only when the timing log is on) and sums them, which is
// what makes "draws per second" mean what it says.
struct CounterAccum {
    uint64_t gpu_submit_us, gpu_draw_us, gpu_fencewait_us, gpu_resolve_us;
    uint64_t gpu_pipeline_us, gpu_pipeline_create_us, gpu_pipeline_create_n;
    uint64_t gpu_texupload_us, gpu_texupload_n;
    uint64_t gpu_waitregmem_us, gpu_waitregmem_n, gpu_cmdwait_us;
    uint64_t dispatched, interrupts;
    uint64_t tex_hit, tex_miss, pipe_hit, pipe_miss, draws, verts;
    uint64_t cmdbuf_stalls, crit_contentions;
};
CounterAccum g_ctr{};

#define CTR(field) static_cast<unsigned long long>(g_ctr.field)

// The GPU timing counters are the fork's SDK's: the stock SDK's CounterId has
// none of them, and there these columns of the log stay 0. A template on the
// enum so the names are only looked up on an SDK that has them.
template <typename Id, typename AddDelta>
static void AddGpuTimingDeltas(AddDelta&& add_delta) {
    if constexpr (requires {
                      Id::kGpuSubmitTimeUs;
                      Id::kGpuDrawTimeUs;
                      Id::kGpuFenceWaitTimeUs;
                      Id::kGpuResolveTimeUs;
                      Id::kGpuPipelineTimeUs;
                      Id::kGpuPipelineCreateTimeUs;
                      Id::kGpuPipelineCreateCount;
                      Id::kGpuTextureUploadTimeUs;
                      Id::kGpuTextureUploadCount;
                      Id::kGpuWaitRegMemTimeUs;
                      Id::kGpuWaitRegMemCount;
                      Id::kGpuCommandWaitTimeUs;
                  }) {
        add_delta(g_ctr.gpu_submit_us,          Id::kGpuSubmitTimeUs);
        add_delta(g_ctr.gpu_draw_us,            Id::kGpuDrawTimeUs);
        add_delta(g_ctr.gpu_fencewait_us,       Id::kGpuFenceWaitTimeUs);
        add_delta(g_ctr.gpu_resolve_us,         Id::kGpuResolveTimeUs);
        add_delta(g_ctr.gpu_pipeline_us,        Id::kGpuPipelineTimeUs);
        add_delta(g_ctr.gpu_pipeline_create_us, Id::kGpuPipelineCreateTimeUs);
        add_delta(g_ctr.gpu_pipeline_create_n,  Id::kGpuPipelineCreateCount);
        add_delta(g_ctr.gpu_texupload_us,       Id::kGpuTextureUploadTimeUs);
        add_delta(g_ctr.gpu_texupload_n,        Id::kGpuTextureUploadCount);
        // The guest CPU blocking on the GPU. If frame time is unaccounted for and
        // nothing is being drawn, this is where to look first.
        add_delta(g_ctr.gpu_waitregmem_us,      Id::kGpuWaitRegMemTimeUs);
        add_delta(g_ctr.gpu_waitregmem_n,       Id::kGpuWaitRegMemCount);
        add_delta(g_ctr.gpu_cmdwait_us,         Id::kGpuCommandWaitTimeUs);
    }
}

void SampleCounters() {
    using rex::perf::CounterId;
    using rex::perf::GetCounter;

    static int64_t s_prev_counters[static_cast<size_t>(CounterId::kCount)];
    static bool s_initialized = false;
    if (!s_initialized) {
        for (size_t i = 0; i < static_cast<size_t>(CounterId::kCount); ++i) {
            s_prev_counters[i] = -1;
        }
        s_initialized = true;
    }

    auto add_delta = [](uint64_t& dst, CounterId id) {
        const size_t idx = static_cast<size_t>(id);
        if (idx >= static_cast<size_t>(CounterId::kCount)) return;
        const int64_t cur = GetCounter(id);
        const int64_t prev = s_prev_counters[idx];
        s_prev_counters[idx] = cur;

        if (prev < 0) {
            // First frame baseline: record initial value without adding full lifetime elapsed
            return;
        }
        if (cur >= prev) {
            dst += static_cast<uint64_t>(cur - prev);
        } else if (cur >= 0) {
            // Counter was reset by runtime between frames
            dst += static_cast<uint64_t>(cur);
        }
    };

    AddGpuTimingDeltas<CounterId>(add_delta);
    // Raw guest CPU churn: a collapse with flat GPU counters and a rising
    // dispatch count is guest code, not the emulator.
    add_delta(g_ctr.dispatched,             CounterId::kFunctionsDispatched);
    add_delta(g_ctr.interrupts,             CounterId::kInterruptDispatches);
    add_delta(g_ctr.tex_hit,                CounterId::kTextureCacheHits);
    add_delta(g_ctr.tex_miss,               CounterId::kTextureCacheMisses);
    add_delta(g_ctr.pipe_hit,               CounterId::kPipelineCacheHits);
    add_delta(g_ctr.pipe_miss,              CounterId::kPipelineCacheMisses);
    add_delta(g_ctr.cmdbuf_stalls,          CounterId::kCommandBufferStalls);
    add_delta(g_ctr.crit_contentions,       CounterId::kCriticalRegionContentions);

    // Guest draw stats: recorded directly at D3DDevice draw entrypoints, ensuring accurate
    // counts even when SDK REXGLUE_ENABLE_PERF_COUNTERS is compiled out.
    g_ctr.draws += mcla::draw_stats::g_draw_calls.exchange(0, std::memory_order_relaxed);
    g_ctr.verts += mcla::draw_stats::g_vertices.exchange(0, std::memory_order_relaxed);
}

// GPU interrupt probe. Diagnostic only: everything below is gated on the timing
// log, so a normal run pays one already-resolved bool test per interrupt.
//
// source 0 = vblank (the runtime's vsync worker), source 1 = a PM4
// PACKET3_INTERRUPT in the guest command stream. Splitting them is what
// distinguishes a runaway vblank catch-up loop from a corrupt command buffer.
//
// g_int_poison counts the game's OWN corruption verdict: its handler compares
// [[user_data+0x2A94]+0x10] against 0x0BADF00D and, on a match, prints
// "Unanticipated CPU_INTERRUPT.  Sign of a corrupt command buffer?" (string at
// 0x82061AB8, referenced from 0x824114AC).
std::atomic<uint64_t> g_int_vblank{0};
std::atomic<uint64_t> g_int_cpu{0};
std::atomic<uint64_t> g_int_poison{0};

void MCLA_GuestInterruptProbe(PPCRegister& r3, PPCRegister& r31) {
    if (!TimingLogEnabled()) return;

    if (static_cast<uint32_t>(r3.u32) != 1) {
        g_int_vblank.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_int_cpu.fetch_add(1, std::memory_order_relaxed);

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t user_data = static_cast<uint32_t>(r31.u32);
    if (!user_data) return;
    const uint32_t ctx = ReadGuestU32(base, user_data + 0x2A94);
    if (!ctx) return;
    if (ReadGuestU32(base, ctx + 0x10) == 0x0BADF00Du)
        g_int_poison.fetch_add(1, std::memory_order_relaxed);
}

void RecordFrameTime() {
    if (!TimingLogEnabled()) return;

    // Every static below is non-atomic, so confine the bookkeeping to the
    // thread that first got here rather than racing across callers.
    static const std::thread::id owner = std::this_thread::get_id();
    if (std::this_thread::get_id() != owner) return;

    // One file per run: a fixed name opened with "w" would destroy the
    // previous capture on the next launch.
    static std::FILE* log = [] () -> std::FILE* {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char name[160];
        std::snprintf(name, sizeof(name),
                      "logs/timing_%04d%02d%02d_%02d%02d%02d_cap%d.log",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      int(REXCVAR_GET(fps_limit)));
        return std::fopen(name, "w");
    }();
    if (!log) return;

    SampleCounters();

    static uint64_t last = 0, frames = 0, last_report = 0;
    static uint64_t start = 0, last_hist = 0;
    static uint32_t spikes[4] = {};
    static uint32_t hist[kHistBuckets] = {};
    static uint64_t hist_frames = 0, hist_total_us = 0;
    static float prev_accum_a = 0.0f, prev_accum_b = 0.0f;

    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (last != 0) {
        const uint64_t d = now - last;
        if (d > 100000) spikes[3]++;
        else if (d > 50000) spikes[2]++;
        else if (d > 33000) spikes[1]++;
        else if (d > 20000) spikes[0]++;

        int bucket = static_cast<int>(d / 1000);
        if (bucket >= kHistBuckets) bucket = kHistBuckets - 1;
        hist[bucket]++;
        hist_frames++;
        hist_total_us += d;
    }
    last = now;
    if (start == 0) { start = now; last_hist = now; last_report = now; }

    bool wrote = false;
    frames++;

    if (now - last_report >= 1000000) {
        wrote = true;
        auto* membase = rex::Runtime::instance()->virtual_membase();

        // Use the ACTUAL elapsed interval, not an assumed 1000 ms: the report
        // fires on the first frame at or past the boundary, so the real window
        // is typically 1000-1035 ms and assuming 1000 biases the mean low.
        const double window_ms = (now - last_report) / 1000.0;
        const double measured_dt_ms = frames ? window_ms / frames : 0.0;
        const float engine_dt  = membase ? ReadGuestF32(membase, kGuestFrameDelta) : 0.0f;
        const float engine_fps = membase ? ReadGuestF32(membase, kGuestFrameRate) : 0.0f;

        std::fprintf(log,
            "[%6.1fs] fps=%llu  spikes: >20ms=%u >33ms=%u >50ms=%u >100ms=%u"
            "  | measured_dt=%.2fms  engine_dt=%.2fms (%.1f fps)  ratio=%.2f\n",
            (now - start) / 1e6, static_cast<unsigned long long>(frames),
            spikes[0], spikes[1], spikes[2], spikes[3],
            measured_dt_ms, engine_dt * 1000.0f, engine_fps,
            (engine_dt > 0.0f) ? (measured_dt_ms / (engine_dt * 1000.0f)) : 0.0);

        // Where the frame actually went. Totals are PER SECOND, accumulated in
        // SampleCounters() every frame, because the counters are reset each
        // frame by the runtime - reading them once at the report boundary gave
        // a single-frame spot check that read `draws=0` on healthy 61 fps
        // seconds purely because the sample landed on an idle frame.
        //
        // These separate a GPU-bound collapse (fencewait, submit), a
        // shader/pipeline stall (pipeline create), streaming thrash (texupload,
        // cache misses), the guest CPU blocking on a GPU register wait
        // (waitregmem, cmdwait), and raw guest CPU churn (dispatched).
        {
            std::fprintf(log,
                "           gpu us/s: submit=%llu draw=%llu fencewait=%llu resolve=%llu"
                " pipeline=%llu (create %llu us x%llu)  texupload=%llu us x%llu\n",
                CTR(gpu_submit_us), CTR(gpu_draw_us), CTR(gpu_fencewait_us),
                CTR(gpu_resolve_us), CTR(gpu_pipeline_us),
                CTR(gpu_pipeline_create_us), CTR(gpu_pipeline_create_n),
                CTR(gpu_texupload_us), CTR(gpu_texupload_n));
            std::fprintf(log,
                "           wait/s: waitregmem=%llu us x%llu  cmdwait=%llu us"
                "  | guest: dispatched=%llu interrupts=%llu"
                " (vblank=%llu cpu=%llu poison=%llu)\n",
                CTR(gpu_waitregmem_us), CTR(gpu_waitregmem_n),
                CTR(gpu_cmdwait_us), CTR(dispatched), CTR(interrupts),
                static_cast<unsigned long long>(
                    g_int_vblank.exchange(0, std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_int_cpu.exchange(0, std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_int_poison.exchange(0, std::memory_order_relaxed)));
            std::fprintf(log,
                "           cache/s: tex hit=%llu miss=%llu | pipeline hit=%llu miss=%llu"
                " | draws=%llu verts=%llu\n",
                CTR(tex_hit), CTR(tex_miss), CTR(pipe_hit), CTR(pipe_miss),
                CTR(draws), CTR(verts));
            // The three host-side spin/yield patches, measured where they decide.
            // fence_hits  = times the guest reached the D3D fence poll body
            // fence_sw    = SwitchToThread() calls the throttle made, and the
            //               wall time they actually cost
            // limiter     = wall time EnforceFrameLimit burned in its PAUSE spin
            std::fprintf(log,
                "           spin/s: fence_hits=%llu fence_sw=%llu (%llu us)"
                "  limiter_spin=%llu us x%llu\n",
                (unsigned long long)g_fence_hook_calls.exchange(0, std::memory_order_relaxed),
                (unsigned long long)g_fence_switches.exchange(0, std::memory_order_relaxed),
                (unsigned long long)g_fence_switch_us.exchange(0, std::memory_order_relaxed),
                (unsigned long long)g_limiter_spin_us.exchange(0, std::memory_order_relaxed),
                (unsigned long long)g_limiter_spins.exchange(0, std::memory_order_relaxed));
            std::fprintf(log,
                "           sys: cmdbuf_stalls=%llu crit_contentions=%llu"
                " | now: apc_depth=%lld threads=%lld qdepth=%lld audio_lat_us=%lld\n",
                CTR(cmdbuf_stalls), CTR(crit_contentions),
                (long long)rex::perf::GetCounter(rex::perf::CounterId::kApcQueueDepth),
                (long long)rex::perf::GetCounter(rex::perf::CounterId::kActiveThreads),
                (long long)rex::perf::GetCounter(rex::perf::CounterId::kBufferQueueDepth),
                (long long)rex::perf::GetCounter(rex::perf::CounterId::kAudioFrameLatencyUs));
            g_ctr = {};
        }

        // Do the engine's accumulated-time totals still advance? A NEGATIVE
        // delta means the engine reset them at a level or race transition, not
        // that they stalled - so only flag near-zero-with-no-change.
        const float accum_a = membase ? ReadGuestF32(membase, kGuestAccumA) : 0.0f;
        const float accum_b = membase ? ReadGuestF32(membase, kGuestAccumB) : 0.0f;
        const float da = accum_a - prev_accum_a;
        const int32_t sub = g_substep_last.load(std::memory_order_relaxed);
        std::fprintf(log,
            "           ACCUM [r3+20]=%.4f (+%.4f/s)  [r3+24]=%.4f (+%.4f/s)"
            "  timescale=%.3f  dt clamp=[%.4f..%.4f]  substep r24=%d (%d passes)"
            "  fixedstep=%llu  %s\n",
            accum_a, da, accum_b, accum_b - prev_accum_b,
            membase ? ReadGuestF32(membase, kGuestTimeScale) : 0.0f,
            membase ? ReadGuestF32(membase, kGuestDtMin) : 0.0f,
            membase ? ReadGuestF32(membase, kGuestDtMax) : 0.0f,
            sub, sub >= 0 ? sub + 1 : -1,
            static_cast<unsigned long long>(
                g_fixedstep_hits.exchange(0, std::memory_order_relaxed)),
            (da >= -0.001f && da < 0.001f) ? "<-- FROZEN" : "");

        prev_accum_a = accum_a;
        prev_accum_b = accum_b;
        frames = 0;
        last_report = now;
        spikes[0] = spikes[1] = spikes[2] = spikes[3] = 0;
    }

    if (now - last_hist >= kHistogramWindowSec * 1000000) {
        wrote = true;
        uint32_t peak = 1;
        for (int i = 0; i < kHistBuckets; ++i)
            if (hist[i] > peak) peak = hist[i];

        std::fprintf(log,
            "\n--- frame-time histogram | window %.1fs..%.1fs | %llu frames | mean %.2f ms ---\n",
            (last_hist - start) / 1e6, (now - start) / 1e6,
            static_cast<unsigned long long>(hist_frames),
            hist_frames ? (hist_total_us / 1000.0 / hist_frames) : 0.0);

        for (int i = 0; i < kHistBuckets; ++i) {
            if (!hist[i]) continue;  // omit empty buckets so clustering shows
            int bar = static_cast<int>(48.0 * hist[i] / peak);
            if (bar < 1) bar = 1;
            std::fprintf(log, "%s%3d ms | %6u %.*s\n",
                         i == kHistBuckets - 1 ? ">=" : "  ", i, hist[i],
                         bar, "################################################");
        }
        std::fprintf(log, "\n");

        for (int i = 0; i < kHistBuckets; ++i) hist[i] = 0;
        hist_frames = 0;
        hist_total_us = 0;
        last_hist = now;
    }

    // Flush only on frames that actually wrote. Flushing every frame would put
    // a blocking disk write in the hot path - the bug this was rewritten to
    // avoid in the midnightclub fork.
    if (wrote) std::fflush(log);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void MCLA_GuestInterruptProbe(PPCRegister& r3, PPCRegister& r31) {}
#endif // REXGLUE_HAS_XEO3_TARGET
