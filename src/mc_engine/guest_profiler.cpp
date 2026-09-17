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
#include <tlhelp32.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#pragma comment(lib, "shlwapi.lib")

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
// What counts as a slow frame, in ms. 20 is right for a 60 FPS target and far
// too loose for anything higher: at a 144 cap the budget is 6.9 ms, so a 16 ms
// frame is already a bad frame while sitting well under a 20 ms threshold.
// Override with MCLA_PROFILE_SLOW_MS.
double SlowFrameMs() {
    static const double ms = [] {
        if (const char* e = std::getenv("MCLA_PROFILE_SLOW_MS")) {
            const double v = std::atof(e);
            if (v > 0.5 && v < 1000.0) return v;
        }
        return 20.0;
    }();
    return ms;
}

// Call stacks are captured only during slow frames, which bounds the memory
// and is the only window we care about.
constexpr int kMaxDepth = 16;

struct Stack {
    uint32_t n = 0;
    uint64_t f[kMaxDepth] = {};
};

std::atomic<bool> g_running{false};
std::atomic<bool> g_frame_is_slow{false};
HANDLE g_target = nullptr;
std::string g_target_name;
std::thread g_sampler;

std::mutex g_mtx;
std::vector<uint64_t> g_samples;       // leaf address of every sample
std::vector<uint64_t> g_slow_samples;  // leaf address, slow frames only
std::vector<Stack> g_slow_stacks;      // full stacks, slow frames only
uint64_t g_total_slow_frames = 0;
uint64_t g_total_frames = 0;

std::FILE* g_log = nullptr;
int g_report_index = 0;

std::string ThreadName(HANDLE h);

// MCLA_PROFILE_THREAD picks which thread the stack sampler watches, matched as
// a case-insensitive substring of the thread name. The per-thread CPU table
// showed the frame-driving thread is mostly idle while other threads sit at
// 99%, so the default target is rarely the interesting one. Guest thread names
// carry a stable handle in parentheses, e.g. "XThread4020 (F80006B8)", and the
// handle is the part that stays the same between runs.
std::string TargetThreadFilter() {
    const char* e = std::getenv("MCLA_PROFILE_THREAD");
    return e ? std::string(e) : std::string();
}

bool EnabledImpl() {
    const char* e = std::getenv("MCLA_PROFILE");
    return e && *e == '1';
}

// Unwind a captured context with RtlVirtualUnwind rather than StackWalk64.
// dbghelp is not thread safe and takes a lock; calling it while the target
// thread is suspended can deadlock if that thread happens to hold the same
// lock. RtlLookupFunctionEntry and RtlVirtualUnwind read the module's static
// unwind tables instead, which is the supported way to walk a suspended
// thread in-process.
int CaptureStack(CONTEXT ctx, uint64_t* out, int max_depth) {
    int n = 0;
    while (n < max_depth && ctx.Rip) {
        out[n++] = ctx.Rip;

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!rf) break;  // leaf function with no unwind info; stop here

        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        const DWORD64 prev_sp = ctx.Rsp;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, rf, &ctx, &handler_data,
                         &establisher, nullptr);
        // A frame that does not move the stack pointer forward means the
        // unwind data is wrong for this address; stop rather than spin.
        if (ctx.Rsp <= prev_sp) break;
    }
    return n;
}

void SamplerLoop() {
    // Below normal so the sampler never competes with the thread it measures.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::vector<uint64_t> local;
    std::vector<uint64_t> local_slow;
    std::vector<Stack> local_stacks;
    local.reserve(4096);
    local_slow.reserve(4096);
    local_stacks.reserve(2048);

    while (g_running.load(std::memory_order_relaxed)) {
        const bool want_stack = g_frame_is_slow.load(std::memory_order_relaxed);

        if (SuspendThread(g_target) != DWORD(-1)) {
            alignas(16) CONTEXT ctx{};
            // CONTEXT_FULL, not CONTEXT_CONTROL: unwinding needs the integer
            // registers, because callee-saved values live in them.
            ctx.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(g_target, &ctx) != FALSE;

            Stack st;
            if (ok && want_stack) st.n = uint32_t(CaptureStack(ctx, st.f, kMaxDepth));

            ResumeThread(g_target);

            if (ok && ctx.Rip) {
                local.push_back(ctx.Rip);
                if (want_stack) {
                    local_slow.push_back(ctx.Rip);
                    if (st.n) local_stacks.push_back(st);
                }
            }
        }

        // Hand the batch over rarely rather than locking every sample.
        if (local.size() >= 2048) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_samples.insert(g_samples.end(), local.begin(), local.end());
            g_slow_samples.insert(g_slow_samples.end(), local_slow.begin(), local_slow.end());
            g_slow_stacks.insert(g_slow_stacks.end(), local_stacks.begin(), local_stacks.end());
            local.clear();
            local_slow.clear();
            local_stacks.clear();
        }

        std::this_thread::sleep_for(std::chrono::microseconds(kSampleIntervalUs));
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_samples.insert(g_samples.end(), local.begin(), local.end());
    g_slow_samples.insert(g_slow_samples.end(), local_slow.begin(), local_slow.end());
    g_slow_stacks.insert(g_slow_stacks.end(), local_stacks.begin(), local_stacks.end());
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

struct Resolved {
    std::string sym;
    std::string mod;
};

class Resolver {
  public:
    const Resolved& operator()(uint64_t addr) {
        auto it = cache_.find(addr);
        if (it != cache_.end()) return it->second;

        Resolved r;
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) &&
            mod) {
            char full[MAX_PATH];
            if (GetModuleFileNameA(mod, full, MAX_PATH)) {
                const char* b = std::strrchr(full, '\\');
                r.mod = b ? b + 1 : full;
            }
        }

        char buf[sizeof(SYMBOL_INFO) + 512];
        std::memset(buf, 0, sizeof(buf));
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 512;
        DWORD64 disp = 0;
        r.sym = SymFromAddr(GetCurrentProcess(), addr, &disp, si) ? si->Name : "<no symbol>";

        return cache_.emplace(addr, std::move(r)).first->second;
    }

  private:
    std::unordered_map<uint64_t, Resolved> cache_;
};

// ntdll and the kernel stubs are never the answer to "who is waiting"; they
// are the wait itself. The first frame outside them is the caller worth
// naming.
bool IsSystemModule(const std::string& m) {
    return _stricmp(m.c_str(), "ntdll.dll") == 0 || _stricmp(m.c_str(), "KERNELBASE.dll") == 0 ||
           _stricmp(m.c_str(), "KERNEL32.DLL") == 0 || _stricmp(m.c_str(), "kernel32.dll") == 0;
}

struct Bucket {
    uint64_t total = 0;
    uint64_t slow = 0;
    uint32_t guest = 0;
};

// Per-thread CPU time.
//
// The stack sampler only watches the thread that drives the frame hook, which
// is the game's main loop. That thread turned out to spend most of a slow
// frame blocked in the guest's own wait on "[MC] Render Thread", so the work
// is somewhere else entirely. GetThreadTimes over every thread in the process
// costs nothing and says which one is actually burning cycles, without having
// to guess which one to sample.
std::unordered_map<DWORD, uint64_t> g_thread_prev;  // tid -> cumulative 100ns

std::string ThreadName(HANDLE h) {
    PWSTR desc = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
        char buf[128];
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, int(sizeof(buf)), nullptr, nullptr);
        if (n > 0) out.assign(buf, size_t(n - 1));
        LocalFree(desc);
    }
    return out;
}

void WriteThreadCpuTable(double window_sec) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    const DWORD me = GetCurrentProcessId();
    struct Row {
        DWORD tid;
        double ms;
        std::string name;
    };
    std::vector<Row> rows;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != me) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;

            FILETIME c{}, e{}, k{}, u{};
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                const uint64_t total =
                    ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                    ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
                auto it = g_thread_prev.find(te.th32ThreadID);
                const uint64_t prev = it == g_thread_prev.end() ? total : it->second;
                g_thread_prev[te.th32ThreadID] = total;
                rows.push_back({te.th32ThreadID, double(total - prev) / 10000.0, ThreadName(h)});
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

    std::fprintf(g_log, "\n--- CPU time per thread over the last %.1f s (one core = 100%%) ---\n",
                 window_sec);
    int n = 0;
    for (const Row& r : rows) {
        if (r.ms < 1.0 && n >= 8) continue;
        if (n++ >= 16) break;
        std::fprintf(g_log, "%8.1f ms  %6.1f%%  tid %-6lu  %s\n", r.ms,
                     window_sec > 0.0 ? 100.0 * r.ms / (window_sec * 1000.0) : 0.0,
                     (unsigned long)r.tid, r.name.empty() ? "<unnamed>" : r.name.c_str());
    }
}

void WriteReport(const char* reason, std::vector<uint64_t> all, std::vector<uint64_t> slow,
                 std::vector<Stack> stacks, uint64_t frames, uint64_t slow_frames) {
    if (!g_log) return;
    if (all.empty()) {
        std::fprintf(g_log, "\n=== %s: no samples ===\n", reason);
        std::fflush(g_log);
        return;
    }

    WriteThreadCpuTable(kReportIntervalSec);

    Resolver resolve;
    std::unordered_map<std::string, Bucket> by_symbol;

    for (uint64_t a : all) {
        const std::string& n = resolve(a).sym;
        Bucket& b = by_symbol[n];
        b.total++;
        if (!b.guest) b.guest = GuestAddrFromSymbol(n.c_str());
    }
    for (uint64_t a : slow) by_symbol[resolve(a).sym].slow++;

    std::vector<std::pair<std::string, Bucket>> rows(by_symbol.begin(), by_symbol.end());
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
                 (unsigned long long)slow_frames, SlowFrameMs(),
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

    // Who is doing the waiting.
    //
    // The leaf of a blocked sample is always some ntdll wait stub, which says
    // nothing. What matters is the first frame above it that belongs to this
    // process: that is the code that decided to block.
    if (!stacks.empty()) {
        std::unordered_map<std::string, uint64_t> callers;
        std::unordered_map<std::string, Stack> exemplar;
        uint64_t attributed = 0;

        for (const Stack& s : stacks) {
            for (uint32_t i = 0; i < s.n; ++i) {
                const Resolved& r = resolve(s.f[i]);
                if (r.mod.empty() || IsSystemModule(r.mod)) continue;
                std::string key = r.mod + "!" + r.sym;
                callers[key]++;
                attributed++;
                exemplar.emplace(key, s);
                break;
            }
        }

        std::vector<std::pair<std::string, uint64_t>> crows(callers.begin(), callers.end());
        std::sort(crows.begin(), crows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::fprintf(g_log,
                     "\n--- slow-frame stacks: first non-system frame (%llu of %llu stacks "
                     "attributed) ---\n",
                     (unsigned long long)attributed, (unsigned long long)stacks.size());
        int c = 0;
        for (const auto& r : crows) {
            if (c++ >= 12) break;
            std::fprintf(g_log, "%6.2f%%  %s\n", 100.0 * double(r.second) / double(stacks.size()),
                         r.first.c_str());
        }

        // Print the single most common stack in full, which is usually enough
        // to read the whole blocking path at a glance.
        if (!crows.empty()) {
            auto it = exemplar.find(crows[0].first);
            if (it != exemplar.end()) {
                std::fprintf(g_log, "\nmost common slow-frame stack (%s):\n", crows[0].first.c_str());
                for (uint32_t i = 0; i < it->second.n; ++i) {
                    const Resolved& r = resolve(it->second.f[i]);
                    const uint32_t g = GuestAddrFromSymbol(r.sym.c_str());
                    if (g)
                        std::fprintf(g_log, "   #%02u %s!%s   (guest 0x%08X)\n", i,
                                     r.mod.empty() ? "?" : r.mod.c_str(), r.sym.c_str(), g);
                    else
                        std::fprintf(g_log, "   #%02u %s!%s\n", i,
                                     r.mod.empty() ? "?" : r.mod.c_str(), r.sym.c_str());
                }
            }
        }
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
        std::string target_name = "the frame-driving thread";

        // Prefer an explicitly named thread when one is requested, because the
        // thread that drives frames is usually not the busy one.
        const std::string filter = TargetThreadFilter();
        if (!filter.empty()) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                const DWORD me = GetCurrentProcessId();
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (Thread32First(snap, &te)) {
                    do {
                        if (te.th32OwnerProcessID != me) continue;
                        HANDLE h = OpenThread(
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, te.th32ThreadID);
                        if (!h) continue;
                        const std::string nm = ThreadName(h);
                        if (!nm.empty() && StrStrIA(nm.c_str(), filter.c_str())) {
                            dup = h;
                            target_name = nm;
                            break;
                        }
                        CloseHandle(h);
                    } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
            }
            if (!dup)
                MC_WARN("[profiler] no thread matched MCLA_PROFILE_THREAD='{}'; using the "
                        "frame-driving thread",
                        filter);
        }

        if (!dup && !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                     &dup, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
            MC_WARN("[profiler] could not duplicate the guest thread handle; profiling is off");
            return;
        }
        g_target = dup;
        g_target_name = target_name;

        // Warm the unwind-table lookup on this thread's own return address, so
        // the first call from the sampler is not the one doing any lazy setup
        // while the target is suspended.
        {
            DWORD64 base = 0;
            RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&Tick), &base, nullptr);
        }

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
                         "the guest addr column can be opened directly in the IDB.\n"
                         "call stacks are captured during slow frames only.\n"
                         "sampling: %s\n",
                         1000000 / kSampleIntervalUs, kReportIntervalSec, SlowFrameMs(),
                         g_target_name.c_str());
            std::fflush(g_log);
        }

        g_samples.reserve(1 << 20);
        g_slow_samples.reserve(1 << 18);
        g_slow_stacks.reserve(1 << 16);
        g_running.store(true, std::memory_order_relaxed);
        g_sampler = std::thread(SamplerLoop);
        MC_INFO("[profiler] sampling the guest thread at {} Hz -> {}", 1000000 / kSampleIntervalUs,
                name);
    }

    g_frame_is_slow.store(frame_ms >= SlowFrameMs(), std::memory_order_relaxed);
    g_total_frames++;
    if (frame_ms >= SlowFrameMs()) g_total_slow_frames++;

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
    std::vector<Stack> stacks;
    uint64_t frames, slow_frames;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        all.swap(g_samples);
        slow.swap(g_slow_samples);
        stacks.swap(g_slow_stacks);
        g_samples.reserve(1 << 20);
        g_slow_samples.reserve(1 << 18);
        g_slow_stacks.reserve(1 << 16);
        frames = g_total_frames;
        slow_frames = g_total_slow_frames;
        g_total_frames = 0;
        g_total_slow_frames = 0;
    }
    WriteReport(reason, std::move(all), std::move(slow), std::move(stacks), frames, slow_frames);
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
