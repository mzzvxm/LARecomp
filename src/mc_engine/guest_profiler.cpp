#ifndef REXGLUE_HAS_XEO3_TARGET

#include "guest_profiler.h"

#include "logging.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace mc::profiler {
namespace {

// 1 kHz. Fast enough to resolve a 16 ms frame into ~16 samples, slow enough
// that suspending the target thread costs a fraction of a percent.
constexpr int kSampleIntervalUs = 1000;

// Reports go out on this cadence so a session produces a timeline rather than
// one average that smears the whole route together.
constexpr double kReportIntervalSec = 30.0;

// A frame at or above this is "slow". Samples taken during slow frames are
// counted separately, which is the number that matters: the average includes
// all the frames that were already fine.
constexpr double kSlowFrameMs = 20.0;

std::atomic<bool> g_running{false};
std::atomic<bool> g_frame_is_slow{false};
HANDLE g_target = nullptr;
std::thread g_sampler;

std::mutex g_mtx;
std::vector<uint64_t> g_samples;       // every sample
std::vector<uint64_t> g_slow_samples;  // samples taken during slow frames
uint64_t g_total_slow_frames = 0;
uint64_t g_total_frames = 0;

std::FILE* g_log = nullptr;
int g_report_index = 0;

bool EnabledImpl() {
    const char* e = std::getenv("MCLA_PROFILE");
    return e && *e == '1';
}

void SamplerLoop() {
    // Below normal so the sampler never competes with the thread it measures.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::vector<uint64_t> local;
    std::vector<uint64_t> local_slow;
    local.reserve(4096);
    local_slow.reserve(4096);

    while (g_running.load(std::memory_order_relaxed)) {
        if (SuspendThread(g_target) != DWORD(-1)) {
            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(g_target, &ctx) != FALSE;
            ResumeThread(g_target);
            if (ok && ctx.Rip) {
                local.push_back(ctx.Rip);
                if (g_frame_is_slow.load(std::memory_order_relaxed))
                    local_slow.push_back(ctx.Rip);
            }
        }

        // Hand the batch over rarely rather than locking every sample.
        if (local.size() >= 2048) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_samples.insert(g_samples.end(), local.begin(), local.end());
            g_slow_samples.insert(g_slow_samples.end(), local_slow.begin(), local_slow.end());
            local.clear();
            local_slow.clear();
        }

        std::this_thread::sleep_for(std::chrono::microseconds(kSampleIntervalUs));
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_samples.insert(g_samples.end(), local.begin(), local.end());
    g_slow_samples.insert(g_slow_samples.end(), local_slow.begin(), local_slow.end());
}

// "rex_sub_8226ABCD" -> 0x8226ABCD. Returns 0 when the symbol is not a
// recompiled guest function, which is how host frames stay distinguishable
// from guest ones in the report.
uint32_t GuestAddrFromSymbol(const char* name) {
    if (!name) return 0;
    const char* p = std::strstr(name, "sub_82");
    if (!p) return 0;
    p += 4;  // land on "82..."
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 16);
    if (end != p + 8) return 0;
    return static_cast<uint32_t>(v);
}

struct Bucket {
    uint64_t total = 0;
    uint64_t slow = 0;
    uint32_t guest = 0;
};

void WriteReport(const char* reason, std::vector<uint64_t> all, std::vector<uint64_t> slow,
                 uint64_t frames, uint64_t slow_frames) {
    if (!g_log) return;
    if (all.empty()) {
        std::fprintf(g_log, "\n=== %s: no samples ===\n", reason);
        std::fflush(g_log);
        return;
    }

    // Resolve once per distinct address, not once per sample.
    std::unordered_map<uint64_t, std::string> sym_cache;
    std::unordered_map<std::string, Bucket> by_symbol;

    HANDLE proc = GetCurrentProcess();
    char symbuf[sizeof(SYMBOL_INFO) + 512];

    auto resolve = [&](uint64_t addr) -> const std::string& {
        auto it = sym_cache.find(addr);
        if (it != sym_cache.end()) return it->second;
        std::memset(symbuf, 0, sizeof(symbuf));
        auto* si = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 512;
        DWORD64 disp = 0;
        std::string name;
        if (SymFromAddr(proc, addr, &disp, si)) {
            name = si->Name;
        } else {
            name = "<no symbol>";
        }
        return sym_cache.emplace(addr, std::move(name)).first->second;
    };

    for (uint64_t a : all) {
        const std::string& n = resolve(a);
        Bucket& b = by_symbol[n];
        b.total++;
        if (!b.guest) b.guest = GuestAddrFromSymbol(n.c_str());
    }
    for (uint64_t a : slow) {
        by_symbol[resolve(a)].slow++;
    }

    std::vector<std::pair<std::string, Bucket>> rows(by_symbol.begin(), by_symbol.end());
    // Rank by slow-frame samples when there are any: during a dip, that is the
    // cost we are actually chasing.
    const bool rank_slow = !slow.empty();
    std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        if (rank_slow && a.second.slow != b.second.slow) return a.second.slow > b.second.slow;
        return a.second.total > b.second.total;
    });

    const double all_n = double(all.size());
    const double slow_n = slow.empty() ? 1.0 : double(slow.size());

    std::fprintf(g_log,
                 "\n=== profile #%d (%s) ===\n"
                 "%llu samples over %llu frames; %llu samples during %llu slow frames (>= %.0f ms)\n"
                 "ranked by %s\n\n"
                 "%7s %7s  %-52s %s\n",
                 g_report_index++, reason, (unsigned long long)all.size(),
                 (unsigned long long)frames, (unsigned long long)slow.size(),
                 (unsigned long long)slow_frames, kSlowFrameMs,
                 rank_slow ? "share of SLOW-frame time" : "share of all time", "slow%", "all%",
                 "symbol", "guest addr");

    int printed = 0;
    for (const auto& r : rows) {
        if (printed++ >= 30) break;
        char guest[16] = "-";
        if (r.second.guest) std::snprintf(guest, sizeof(guest), "0x%08X", r.second.guest);
        std::fprintf(g_log, "%6.2f%% %6.2f%%  %-52s %s\n", 100.0 * double(r.second.slow) / slow_n,
                     100.0 * double(r.second.total) / all_n, r.first.c_str(), guest);
    }
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
}

}  // namespace

bool Enabled() {
    static const bool e = EnabledImpl();
    return e;
}

void Tick(double frame_ms) {
    if (!Enabled()) return;

    static bool started = false;
    if (!started) {
        started = true;

        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
            MC_WARN("[profiler] could not duplicate the guest thread handle; profiling is off");
            return;
        }
        g_target = dup;

        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char name[160];
        std::snprintf(name, sizeof(name), "logs/profile_%04d%02d%02d_%02d%02d%02d.log",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                      tm.tm_sec);
        g_log = std::fopen(name, "w");
        if (g_log) {
            std::fprintf(g_log,
                         "LARecomp guest sampling profile\n"
                         "sample rate %d Hz, report every %.0f s, slow frame >= %.0f ms\n"
                         "symbols named rex_sub_82XXXXXX are recompiled guest functions;\n"
                         "the guest addr column can be opened directly in the IDB.\n",
                         1000000 / kSampleIntervalUs, kReportIntervalSec, kSlowFrameMs);
            std::fflush(g_log);
        }

        g_samples.reserve(1 << 20);
        g_slow_samples.reserve(1 << 18);
        g_running.store(true, std::memory_order_relaxed);
        g_sampler = std::thread(SamplerLoop);
        MC_INFO("[profiler] sampling the guest thread at {} Hz -> {}", 1000000 / kSampleIntervalUs,
                name);
    }

    g_frame_is_slow.store(frame_ms >= kSlowFrameMs, std::memory_order_relaxed);
    g_total_frames++;
    if (frame_ms >= kSlowFrameMs) g_total_slow_frames++;

    static double accum = 0.0;
    accum += frame_ms / 1000.0;
    if (accum >= kReportIntervalSec) {
        accum = 0.0;
        Report("periodic");
    }
}

void Report(const char* reason) {
    if (!Enabled() || !g_log) return;

    std::vector<uint64_t> all, slow;
    uint64_t frames, slow_frames;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        all.swap(g_samples);
        slow.swap(g_slow_samples);
        g_samples.reserve(1 << 20);
        g_slow_samples.reserve(1 << 18);
        frames = g_total_frames;
        slow_frames = g_total_slow_frames;
        g_total_frames = 0;
        g_total_slow_frames = 0;
    }
    WriteReport(reason, std::move(all), std::move(slow), frames, slow_frames);
}

void Shutdown() {
    if (!Enabled()) return;
    if (g_running.exchange(false, std::memory_order_relaxed)) {
        if (g_sampler.joinable()) g_sampler.join();
        Report("final");
    }
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (g_target) {
        CloseHandle(g_target);
        g_target = nullptr;
    }
}

}  // namespace mc::profiler

#else  // !_WIN32

namespace mc::profiler {
bool Enabled() { return false; }
void Tick(double) {}
void Report(const char*) {}
void Shutdown() {}
}  // namespace mc::profiler

#endif  // _WIN32
#endif  // REXGLUE_HAS_XEO3_TARGET
