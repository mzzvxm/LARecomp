#ifndef REXGLUE_HAS_XEO3_TARGET

#include "guest_profiler.h"

#include <rex/cvar.h>
#include <rex/ppc/func.h>

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
#include <intrin.h>
#include <shlwapi.h>
#include <tlhelp32.h>

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
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")

// Win10 1803+. Declared here so the build does not depend on the SDK headers
// being new enough; CreateWaitableTimerExW falls back when the flag is refused.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

REXCVAR_DEFINE_BOOL(guest_profile, false, "MCLA/Diag",
    "Sample the busiest threads, and photograph every thread when the game stops. "
    "The same thing MCLA_PROFILE=1 turns on, reachable from the config file so a "
    "session need not be launched from a shell to get it. The periodic report says "
    "where frame time goes; the stall dump is the one that matters when a load "
    "never finishes, because the thread the sampler follows is parked in a wait by "
    "then and the stuck one is somebody else. Reports land in logs/profile_*.log.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mc::profiler {
namespace {

//=============================================================================
// Tunables
//=============================================================================

// Per-target sampling rate. The loop fires once per target per period, so the
// total suspend rate is this times the number of targets -- capped below.
constexpr int kPerTargetRateHz = 1000;

// Ceiling on total suspends per second across all targets. Each suspend costs
// a couple of microseconds and knocks the target off its core, so sampling
// four threads at a full 1 kHz each would start to shape what it measures.
constexpr int kMaxTotalRateHz = 4000;

// How many threads are sampled at once. One is pinned (the frame-driving
// thread, or whatever MCLA_PROFILE_THREAD names); the rest are re-chosen every
// report from the per-thread CPU table, because the busy thread changes with
// what the game is doing and cannot be named at boot -- it often does not exist
// yet when the first frame ticks.
constexpr int kMaxTargets = 4;

// Reports go out on this cadence so a session produces a timeline rather than
// one average that smears the whole route together. Measured against the wall
// clock, not accumulated frame time: a paused or hitching game makes those two
// diverge, and every percentage in the CPU table is computed from the window.
constexpr double kReportIntervalSec = 30.0;

// Call stacks are captured only during slow frames, which bounds the memory
// and is the only window we care about.
constexpr int kMaxDepth = 24;

// A stall is the interesting case and the sampler cannot see it: when a load
// never finishes the sampled threads are parked in a wait while some OTHER
// thread is the one stuck. So once the frame hook has been silent this long,
// every thread in the process is photographed once -- suspended, instruction
// pointer read, resumed -- and written out with symbols.
constexpr double kStallSeconds = 8.0;

// A symbol further than this from the sampled address is not that symbol. See
// Resolver::operator() for why this matters.
constexpr DWORD64 kMaxSymbolDisp = 0x8000;

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

struct Stack {
    uint32_t n = 0;
    uint64_t f[kMaxDepth] = {};
};

//=============================================================================
// Shared with the frame thread
//=============================================================================

std::atomic<int64_t> g_last_tick_ns{0};
std::atomic<bool> g_stall_dumped{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_frame_is_slow{false};

std::mutex g_frame_mtx;
uint64_t g_total_frames = 0;
uint64_t g_total_slow_frames = 0;

std::thread g_sampler;
std::FILE* g_log = nullptr;
int g_report_index = 0;
DWORD g_pinned_tid = 0;
std::string g_pinned_name;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

//=============================================================================
// Guest address recovery, without a PDB
//=============================================================================
//
// Codegen emits PPCFuncMappings[] -- every recompiled function as a
// { guest address, host function pointer } pair, terminated by { 0, nullptr }.
// It is linked into the exe and needs no symbols at all, which matters because
// a profile taken on somebody else's machine has no PDB next to it: every
// recompiled frame in such a log resolves as "<no symbol>" and the guest
// address column, the whole point of the report, comes out empty.
//
// Inverting the table turns a sampled instruction pointer back into the guest
// address it came from. The lookup is keyed on the FUNCTION START reported by
// the unwind tables rather than on the sampled address itself, so an address
// that belongs to no recompiled function is rejected outright instead of being
// charged to whichever entry happens to precede it.

std::vector<std::pair<uint64_t, uint32_t>> g_guest_by_host;  // sorted by host
std::unordered_map<uint64_t, uint32_t> g_guest_exact;        // host start -> guest
std::unordered_set<uint64_t> g_guest_folded;                 // host claimed by 2+ guests
uint64_t g_exe_lo = 0, g_exe_hi = 0;                         // larecomp.exe image

void BuildGuestMap() {
    // Recompiled code only ever lives in the exe. Without this bound the range
    // fallback below charges addresses in ntdll, KERNEL32 and the runtime DLLs
    // to whichever table entry sits nearest in memory -- and since those
    // modules load ABOVE the exe, every one of them came out stamped with the
    // last entry's guest address.
    if (HMODULE exe = GetModuleHandleW(nullptr)) {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                g_exe_lo = reinterpret_cast<uint64_t>(exe);
                g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
            }
        }
    }

    size_t n = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) ++n;
    g_guest_by_host.reserve(n);
    g_guest_exact.reserve(n * 2);

    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
        const uint64_t host = reinterpret_cast<uint64_t>(m->host);
        const uint32_t guest = static_cast<uint32_t>(m->guest);
        g_guest_by_host.emplace_back(host, guest);

        // Release links fold identical functions together (/OPT:ICF), and with
        // 30k recompiled stubs some of them ARE identical. The table then
        // points several guest addresses at one host address, and keeping
        // whichever arrived first would put a confident, wrong number in the
        // report. Those are marked instead, and counted in the header so the
        // ambiguity is visible rather than inferred.
        auto [it, inserted] = g_guest_exact.emplace(host, guest);
        if (!inserted && it->second != guest) g_guest_folded.insert(host);
    }
    std::sort(g_guest_by_host.begin(), g_guest_by_host.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

// 0 when the address is not inside a recompiled guest function.
// `approx` comes back true when the answer is attribution by adjacency or by a
// folded entry rather than an exact hit, so the report can say so instead of
// printing a number that looks as solid as the exact ones.
uint32_t GuestAddrFromRip(uint64_t rip, bool* approx) {
    if (approx) *approx = false;
    if (g_guest_by_host.empty()) return 0;
    if (g_exe_hi && (rip < g_exe_lo || rip >= g_exe_hi)) return 0;

    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(rip, &image_base, nullptr);
    if (!rf) return 0;  // no unwind info: not one of ours

    const uint64_t start = uint64_t(image_base) + rf->BeginAddress;
    if (auto it = g_guest_exact.find(start); it != g_guest_exact.end()) {
        if (approx && g_guest_folded.count(start)) *approx = true;
        return it->second;
    }

    // The unwind entry can describe a FRAGMENT of a function rather than the
    // function itself (chained unwind info, or a block the linker moved). The
    // fragment start is still a real code boundary, so the entry immediately
    // below it in layout order is the function it was split from -- as long as
    // the next recompiled function starts after it.
    //
    // This is attribution by adjacency and it IS sometimes wrong: a non-guest
    // helper laid out after a recompiled function picks up that function's
    // address (measured: std::this_thread::sleep_for came out tagged with the
    // guest address of the hook that calls it). Release builds inline more, so
    // it gets worse there, not better. Hence the marker.
    auto it = std::upper_bound(g_guest_by_host.begin(), g_guest_by_host.end(), start,
                               [](uint64_t v, const auto& e) { return v < e.first; });
    if (it == g_guest_by_host.begin()) return 0;
    --it;
    if ((it + 1) != g_guest_by_host.end() && start >= (it + 1)->first) return 0;
    if (approx) *approx = true;
    return it->second;
}

// "rex_sub_8226ABCD" -> 0x8226ABCD. Secondary source, used when a PDB is
// present and the mapping table missed.
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

//=============================================================================
// Symbolization
//=============================================================================

struct Resolved {
    std::string sym;
    std::string mod;
    uint32_t guest = 0;
    bool guest_approx = false;
};

class Resolver {
  public:
    Resolver() {
        // InstallCrashLogger calls SymInitialize with fInvadeProcess, which
        // enumerates the modules loaded *at that moment*. It runs from the app
        // constructor, long before Runtime::Setup loads the GPU plugin, so
        // rexgpu-xenos is never registered with dbghelp and every sample in it
        // resolved as <no symbol> - over a third of the GPU Commands thread.
        // Refreshing here picks up everything loaded since.
        SymRefreshModuleList(GetCurrentProcess());
    }

    const Resolved& operator()(uint64_t addr) {
        auto it = cache_.find(addr);
        if (it != cache_.end()) return it->second;

        Resolved r;
        uint64_t mod_base = 0;
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) &&
            mod) {
            mod_base = reinterpret_cast<uint64_t>(mod);
            char full[MAX_PATH];
            if (GetModuleFileNameA(mod, full, MAX_PATH)) {
                const char* b = std::strrchr(full, '\\');
                r.mod = b ? b + 1 : full;
            }
        }

        r.guest = GuestAddrFromRip(addr, &r.guest_approx);

        // SymFromAddr returns the nearest PRECEDING public symbol with no
        // bound on the distance. In a module whose private symbols are absent
        // -- ucrtbase, or any module whose PDB is not next to the exe -- every
        // unsymbolized address lands on whatever export happens to sit below
        // it, and the report grows hot spots that do not exist: "fmal" as the
        // fourth heaviest frame, "_report_gsfailure" in every window when a
        // real GS failure would have killed the process. Rejecting a symbol
        // whose reported size does not cover the address, or that is absurdly
        // far away, turns those back into honest module+offset.
        char buf[sizeof(SYMBOL_INFO) + 512];
        std::memset(buf, 0, sizeof(buf));
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 512;
        DWORD64 disp = 0;
        bool named = false;
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
            const bool covered = si->Size ? (disp < si->Size) : (disp <= kMaxSymbolDisp);
            if (covered) {
                r.sym = si->Name;
                named = true;
                if (!r.guest) r.guest = GuestAddrFromSymbol(si->Name);
            }
        }

        if (!named) {
            char tmp[64];
            if (r.guest) {
                // Name it after the guest function; that is the name the user
                // can actually look up, PDB or no PDB.
                std::snprintf(tmp, sizeof(tmp), "guest_sub_%08X", r.guest);
            } else if (mod_base) {
                std::snprintf(tmp, sizeof(tmp), "+0x%llX",
                              (unsigned long long)(addr - mod_base));
            } else {
                std::snprintf(tmp, sizeof(tmp), "0x%llX", (unsigned long long)addr);
            }
            r.sym = tmp;
        }

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

//=============================================================================
// Machine description
//=============================================================================
//
// A profile is usually read on a different machine than it was taken on, and
// the first question about a slow one is whether the box had the cores to run
// it. Without this header the CPU table below is uninterpretable: five cores'
// worth of demand is healthy on twelve cores and a catastrophe on four.

std::string CpuBrand() {
    int r[4] = {};
    __cpuid(r, 0x80000000);
    if (static_cast<unsigned>(r[0]) < 0x80000004u) return "<unknown CPU>";
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s(brand);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t b = s.find_first_not_of(' ');
    return b == std::string::npos ? s : s.substr(b);
}

DWORD PhysicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return 0;
    std::vector<uint8_t> buf(len);
    auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) return 0;

    DWORD n = 0;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (!e->Size) break;
        if (e->Relationship == RelationProcessorCore) ++n;
        off += e->Size;
    }
    return n;
}

std::string GpuName() {
    DISPLAY_DEVICEA dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return dd.DeviceString;
        dd.cb = sizeof(dd);
    }
    return "<unknown GPU>";
}

std::string OsBuild() {
    struct VerW {
        ULONG size, major, minor, build, platform;
        WCHAR csd[128];
    } vi{};
    vi.size = sizeof(vi);
    using Fn = LONG(WINAPI*)(VerW*);
    char out[64] = "<unknown>";
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<Fn>(
                reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
            if (fn(&vi) == 0)
                std::snprintf(out, sizeof(out), "%lu.%lu build %lu", vi.major, vi.minor, vi.build);
        }
    }
    return out;
}

void WriteMachineHeader(std::FILE* f) {
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const DWORD physical = PhysicalCores();

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    const bool have_mem = GlobalMemoryStatusEx(&ms) != FALSE;

    std::fprintf(f,
                 "\n--- machine ---\n"
                 "cpu       %s\n"
                 "cores     %lu physical, %lu logical\n",
                 CpuBrand().c_str(), (unsigned long)physical, (unsigned long)logical);
    if (have_mem)
        std::fprintf(f, "ram       %.1f GiB total, %.1f GiB free\n",
                     double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0),
                     double(ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
    std::fprintf(f,
                 "gpu       %s\n"
                 "os        Windows %s\n"
                 "guest map %zu recompiled functions; %zu host addresses are shared by more\n"
                 "          than one of them (linker folding). A guest address printed with a\n"
                 "          trailing ~ is attribution by adjacency or by a folded entry, not exact.\n",
                 GpuName().c_str(), OsBuild().c_str(), g_guest_by_host.size(),
                 g_guest_folded.size());
}

//=============================================================================
// Per-thread CPU time
//=============================================================================
//
// The stack sampler can only watch a handful of threads. GetThreadTimes over
// every thread in the process costs nothing and says which ones are actually
// burning cycles, so the report uses it for two things: to print the table,
// and to choose what to sample next.

struct CpuRow {
    DWORD tid = 0;
    double ms = 0.0;
    std::string name;
};

std::unordered_map<DWORD, uint64_t> g_thread_prev;  // tid -> cumulative 100ns
int64_t g_cpu_epoch_ns = 0;  // when those figures were last read

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

// The deltas are against whenever this was last called, which is not the same
// span as the report window -- targets get re-chosen a couple of seconds into
// the session. Reporting the CPU table against an assumed 30 s is how every
// percentage in it silently goes wrong.
std::vector<CpuRow> CollectThreadCpu(double* out_window_sec) {
    std::vector<CpuRow> rows;
    const int64_t now = NowNs();
    if (out_window_sec)
        *out_window_sec = g_cpu_epoch_ns ? double(now - g_cpu_epoch_ns) / 1e9 : 0.0;
    g_cpu_epoch_ns = now;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return rows;

    const DWORD me = GetCurrentProcessId();
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
                const uint64_t total = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
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

    std::sort(rows.begin(), rows.end(),
              [](const CpuRow& a, const CpuRow& b) { return a.ms > b.ms; });
    return rows;
}

void WriteThreadCpuTable(const std::vector<CpuRow>& rows, double window_sec) {
    // The total is over EVERY thread, not just the ones printed. Truncating at
    // sixteen rows and printing no total is how a saturated machine hides: the
    // rows that fit looked merely busy.
    double total_ms = 0.0;
    for (const CpuRow& r : rows) total_ms += r.ms;

    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const double cores = window_sec > 0.0 ? total_ms / (window_sec * 1000.0) : 0.0;

    std::fprintf(g_log, "\n--- CPU time per thread over the last %.1f s (one core = 100%%) ---\n",
                 window_sec);
    int n = 0;
    for (const CpuRow& r : rows) {
        if (r.ms < 1.0 && n >= 8) continue;
        if (n++ >= 16) break;
        std::fprintf(g_log, "%8.1f ms  %6.1f%%  tid %-6lu  %s\n", r.ms,
                     window_sec > 0.0 ? 100.0 * r.ms / (window_sec * 1000.0) : 0.0,
                     (unsigned long)r.tid, r.name.empty() ? "<unnamed>" : r.name.c_str());
    }
    std::fprintf(g_log, "%8.1f ms  %6.1f%%  ALL %zu threads = %.2f of %lu cores%s\n", total_ms,
                 window_sec > 0.0 ? 100.0 * total_ms / (window_sec * 1000.0) : 0.0, rows.size(),
                 cores, (unsigned long)logical,
                 (logical && cores > double(logical) * 0.9) ? "   <-- SATURATED" : "");
}

//=============================================================================
// Sample storage (sampler-thread owned; no lock)
//=============================================================================

struct ThreadData {
    std::string name;
    std::vector<uint64_t> all;
    std::vector<uint64_t> slow;
    std::vector<Stack> stacks;
    uint64_t attempts = 0;
};

struct Target {
    HANDLE h = nullptr;
    DWORD tid = 0;
    std::string name;
    bool pinned = false;
};

std::unordered_map<DWORD, ThreadData> g_data;
std::vector<Target> g_targets;

void CloseUnpinnedTargets() {
    for (Target& t : g_targets) {
        if (!t.pinned && t.h) CloseHandle(t.h);
    }
    g_targets.erase(std::remove_if(g_targets.begin(), g_targets.end(),
                                   [](const Target& t) { return !t.pinned; }),
                    g_targets.end());
}

// Re-choose the sampled threads from the CPU table. The pinned target (the
// frame-driving thread, or MCLA_PROFILE_THREAD) always stays: it is the one
// whose stalls define the frame, even when it is idle.
void RefreshTargets(const std::vector<CpuRow>& rows) {
    CloseUnpinnedTargets();

    const DWORD self = GetCurrentThreadId();
    for (const CpuRow& r : rows) {
        if (int(g_targets.size()) >= kMaxTargets) break;
        if (r.tid == self || r.tid == g_pinned_tid) continue;
        if (r.ms < 1.0) break;  // sorted; nothing below is worth a slot

        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, r.tid);
        if (!h) continue;
        g_targets.push_back({h, r.tid, r.name.empty() ? "<unnamed>" : r.name, false});
    }
}

//=============================================================================
// Stack capture
//=============================================================================

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

void DumpAllThreads(const char* reason);
void WriteReport(const char* reason, double window_sec, const std::vector<CpuRow>& rows,
                 double cpu_window_sec);

//=============================================================================
// Sampler
//=============================================================================

void SamplerLoop() {
    // Below normal so the sampler never competes with the threads it measures.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // sleep_for is "at least": with the default timer resolution a 1 ms sleep
    // can be 15 ms, and under load it stretches exactly when the interesting
    // work is happening -- so the samples that go missing are the ones that
    // mattered, and the profile is biased toward looking idle. The measured
    // rate on a real session was under half the 1 kHz the header claimed. A
    // high-resolution waitable timer holds the cadence without raising the
    // process-wide timer resolution, which would change what we are measuring.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    // Prime the CPU counters. The first read has nothing to subtract from, so
    // every delta is zero and no target could be chosen from it.
    CollectThreadCpu(nullptr);
    const int64_t primed_at = NowNs();
    bool targets_chosen = false;

    int64_t window_start = NowNs();

    while (g_running.load(std::memory_order_relaxed)) {
        // Aim for kPerTargetRateHz on each target, but never exceed the total
        // suspend budget. A periodic waitable timer takes its period in whole
        // milliseconds, which cannot express this, so the timer is armed
        // one-shot on each pass instead.
        // Every target is sampled once per pass, so the per-target rate IS the
        // loop rate and does not depend on how many targets there are; what
        // scales with the count is the total number of suspends.
        const int n_targets = int(g_targets.size() ? g_targets.size() : 1);
        const int want_us = 1000000 / kPerTargetRateHz;
        const int floor_us = n_targets * 1000000 / kMaxTotalRateHz;
        const int period_us = want_us > floor_us ? want_us : floor_us;

        const bool want_stack = g_frame_is_slow.load(std::memory_order_relaxed);

        for (Target& t : g_targets) {
            ThreadData& d = g_data[t.tid];
            if (d.name.empty()) d.name = t.name;
            d.attempts++;

            if (SuspendThread(t.h) == DWORD(-1)) continue;

            alignas(16) CONTEXT ctx{};
            // CONTEXT_FULL, not CONTEXT_CONTROL: unwinding needs the integer
            // registers, because callee-saved values live in them.
            ctx.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(t.h, &ctx) != FALSE;

            Stack st;
            if (ok && want_stack) st.n = uint32_t(CaptureStack(ctx, st.f, kMaxDepth));

            ResumeThread(t.h);

            // Nothing above this line allocates: the target is suspended and
            // may be holding the heap lock.
            if (ok && ctx.Rip) {
                d.all.push_back(ctx.Rip);
                if (want_stack) {
                    d.slow.push_back(ctx.Rip);
                    if (st.n) d.stacks.push_back(st);
                }
            }
        }

        // The frame hook is the only thing that moves this, so when it stops
        // moving the game has stopped with it -- and that is the moment worth
        // a picture of every thread. Once only: a stalled game stays stalled.
        const int64_t last = g_last_tick_ns.load(std::memory_order_relaxed);
        if (last && !g_stall_dumped.load(std::memory_order_relaxed)) {
            if (double(NowNs() - last) / 1e9 >= kStallSeconds) {
                g_stall_dumped.store(true, std::memory_order_relaxed);
                DumpAllThreads("STALL -- no frame for 8 s");
            }
        }

        // Fill the remaining slots from the CPU table a couple of seconds in,
        // rather than at the first report. The busy threads are the whole
        // reason for sampling more than one, and waiting a full window to pick
        // them throws the first 30 s away.
        if (!targets_chosen && double(NowNs() - primed_at) / 1e9 >= 2.0) {
            targets_chosen = true;
            RefreshTargets(CollectThreadCpu(nullptr));
        }

        // Reporting runs here, not on the frame thread. Symbolizing ~15k
        // addresses, snapshotting the thread list and writing the file took
        // milliseconds on the very thread being measured, and dbghelp showed
        // up inside its own profile.
        const double window = double(NowNs() - window_start) / 1e9;
        if (window >= kReportIntervalSec) {
            double cpu_window = 0.0;
            std::vector<CpuRow> rows = CollectThreadCpu(&cpu_window);
            WriteReport("periodic", window, rows, cpu_window);
            RefreshTargets(rows);
            window_start = NowNs();
        }

        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -10LL * period_us;  // relative, 100ns units
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, DWORD(period_us / 1000 + 2));
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(period_us));
        }
    }

    if (timer) CloseHandle(timer);
}

//=============================================================================
// Report
//=============================================================================

void WriteThreadSection(Resolver& resolve, const ThreadData& d, DWORD tid, double window_sec,
                        double cpu_ms, bool detailed) {
    // Both rates are measured, not assumed. The old report printed the
    // nominal 1 kHz in its header while actually landing under 500 Hz, which
    // made every "samples over N frames" line quietly wrong.
    const double eff_hz = window_sec > 0.0 ? double(d.all.size()) / window_sec : 0.0;
    const double try_hz = window_sec > 0.0 ? double(d.attempts) / window_sec : 0.0;

    std::fprintf(g_log,
                 "\n  [tid %lu] %s -- %zu samples (%.0f Hz landed of %.0f Hz attempted), "
                 "%.1f%% of a core\n",
                 (unsigned long)tid, d.name.c_str(), d.all.size(), eff_hz, try_hz,
                 window_sec > 0.0 ? 100.0 * cpu_ms / (window_sec * 1000.0) : 0.0);

    if (d.all.empty()) return;

    struct Bucket {
        uint64_t total = 0;
        uint64_t slow = 0;
        uint32_t guest = 0;
        bool guest_approx = false;
    };
    std::unordered_map<std::string, Bucket> by_symbol;
    for (uint64_t a : d.all) {
        const Resolved& r = resolve(a);
        const std::string key = r.mod.empty() ? r.sym : r.mod + "!" + r.sym;
        Bucket& b = by_symbol[key];
        b.total++;
        if (!b.guest) {
            b.guest = r.guest;
            b.guest_approx = r.guest_approx;
        }
    }
    for (uint64_t a : d.slow) {
        const Resolved& r = resolve(a);
        by_symbol[r.mod.empty() ? r.sym : r.mod + "!" + r.sym].slow++;
    }

    std::vector<std::pair<std::string, Bucket>> rows(by_symbol.begin(), by_symbol.end());
    const bool rank_slow = !d.slow.empty();
    std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        if (rank_slow && a.second.slow != b.second.slow) return a.second.slow > b.second.slow;
        return a.second.total > b.second.total;
    });

    const double all_n = double(d.all.size());
    const double slow_n = d.slow.empty() ? 1.0 : double(d.slow.size());

    std::fprintf(g_log, "  %7s %7s  %-56s %s\n", "slow%", "all%", "symbol", "guest addr");
    int printed = 0;
    for (const auto& r : rows) {
        if (printed++ >= (detailed ? 20 : 10)) break;
        char guest[16] = "-";
        if (r.second.guest)
            std::snprintf(guest, sizeof(guest), "0x%08X%s", r.second.guest,
                          r.second.guest_approx ? "~" : "");
        std::fprintf(g_log, "  %6.2f%% %6.2f%%  %-56s %s\n",
                     100.0 * double(r.second.slow) / slow_n,
                     100.0 * double(r.second.total) / all_n, r.first.c_str(), guest);
    }

    if (d.stacks.empty()) return;

    // Who is doing the waiting.
    //
    // The leaf of a blocked sample is always some ntdll wait stub, which says
    // nothing. What matters is the first frame above it that belongs to this
    // process: that is the code that decided to block.
    std::unordered_map<std::string, uint64_t> callers;
    uint64_t attributed = 0;
    for (const Stack& s : d.stacks) {
        for (uint32_t i = 0; i < s.n; ++i) {
            const Resolved& r = resolve(s.f[i]);
            if (r.mod.empty() || IsSystemModule(r.mod)) continue;
            callers[r.mod + "!" + r.sym]++;
            attributed++;
            break;
        }
    }
    std::vector<std::pair<std::string, uint64_t>> crows(callers.begin(), callers.end());
    std::sort(crows.begin(), crows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::fprintf(g_log, "  slow-frame stacks: first non-system frame (%llu of %zu attributed)\n",
                 (unsigned long long)attributed, d.stacks.size());
    int c = 0;
    for (const auto& r : crows) {
        if (c++ >= 8) break;
        std::fprintf(g_log, "  %6.2f%%  %s\n", 100.0 * double(r.second) / double(d.stacks.size()),
                     r.first.c_str());
    }

}

void WriteReport(const char* reason, double window_sec, const std::vector<CpuRow>& rows,
                 double cpu_window_sec) {
    if (!g_log) return;

    std::fprintf(g_log, "\n=== profile #%d (%s) ===\n", g_report_index++, reason);

    WriteThreadCpuTable(rows, cpu_window_sec > 0.0 ? cpu_window_sec : window_sec);

    size_t total_samples = 0;
    for (auto& [tid, d] : g_data) total_samples += d.all.size();
    if (!total_samples) {
        std::fprintf(g_log, "\nno samples\n\n");
        std::fflush(g_log);
        g_data.clear();
        return;
    }

    Resolver resolve;

    std::vector<std::pair<size_t, DWORD>> order;  // sample count, tid
    order.reserve(g_data.size());
    for (auto& [tid, d] : g_data) order.emplace_back(d.all.size(), tid);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::fprintf(g_log, "\n--- sampled threads ---\n");
    int n = 0;
    for (const auto& [count, tid] : order) {
        double cpu_ms = 0.0;
        for (const CpuRow& r : rows) {
            if (r.tid == tid) {
                cpu_ms = r.ms;
                break;
            }
        }
        // Full stack detail for the two heaviest; a symbol table is enough for
        // the rest and keeps the log readable.
        WriteThreadSection(resolve, g_data[tid], tid, window_sec, cpu_ms, n < 2);
        ++n;
    }

    g_data.clear();
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
}

// Every thread in this process, with the function each one is sitting in.
//
// The sampler above follows a few threads, which is the right target for
// "what is slow" and the wrong one for "what is stuck": when a load never
// finishes, those threads are parked in a wait and some OTHER thread is the
// one not moving. One suspend-read-resume pass over every thread names it.
void DumpAllThreads(const char* reason) {
    if (!g_log) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        std::fprintf(g_log, "\n=== %s: cannot enumerate threads ===\n", reason);
        std::fflush(g_log);
        return;
    }

    const DWORD self_pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    Resolver resolve;

    std::fprintf(g_log, "\n=== %s: every thread in the process ===\n%8s  %-56s %s\n", reason,
                 "tid", "symbol", "guest addr");

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != self_pid) continue;
        if (te.th32ThreadID == self_tid) continue;  // the sampler itself

        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, te.th32ThreadID);
        if (!th) continue;

        DWORD64 rip = 0;
        if (SuspendThread(th) != DWORD(-1)) {
            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) rip = ctx.Rip;
            ResumeThread(th);
        }
        const std::string name = ThreadName(th);
        CloseHandle(th);
        if (!rip) continue;

        // Symbolization happens after the resume: dbghelp takes a lock the
        // suspended thread might be holding.
        const Resolved& r = resolve(rip);
        char guest[16] = "-";
        if (r.guest)
            std::snprintf(guest, sizeof(guest), "0x%08X%s", r.guest, r.guest_approx ? "~" : "");
        const std::string sym = (r.mod.empty() ? std::string("?") : r.mod) + "!" + r.sym;
        std::fprintf(g_log, "%8lu  %-56s %s  %s\n", (unsigned long)te.th32ThreadID, sym.c_str(),
                     guest, name.c_str());
    }
    CloseHandle(snap);
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
}

bool EnabledImpl() {
    const char* e = std::getenv("MCLA_PROFILE");
    return (e && *e == '1') || REXCVAR_GET(guest_profile);
}

// MCLA_PROFILE_THREAD pins the sampler to a named thread, matched as a
// case-insensitive substring. Guest thread names carry a stable handle in
// parentheses, e.g. "XThread4020 (F80006B8)", and the handle is the part that
// stays the same between runs. Without it the pinned target is the thread that
// drives frames -- which is usually blocked rather than busy, so the other
// slots are filled from the CPU table instead of guessed at boot.
std::string TargetThreadFilter() {
    const char* e = std::getenv("MCLA_PROFILE_THREAD");
    return e ? std::string(e) : std::string();
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

        BuildGuestMap();

        HANDLE pinned = nullptr;
        std::string target_name = "the frame-driving thread";
        DWORD pinned_tid = GetCurrentThreadId();

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
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                  THREAD_QUERY_LIMITED_INFORMATION,
                                              FALSE, te.th32ThreadID);
                        if (!h) continue;
                        const std::string nm = ThreadName(h);
                        if (!nm.empty() && StrStrIA(nm.c_str(), filter.c_str())) {
                            pinned = h;
                            target_name = nm;
                            pinned_tid = te.th32ThreadID;
                            break;
                        }
                        CloseHandle(h);
                    } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
            }
            if (!pinned)
                MC_WARN("[profiler] no thread matched MCLA_PROFILE_THREAD='{}'; pinning the "
                        "frame-driving thread",
                        filter);
        }

        if (!pinned &&
            !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &pinned,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_LIMITED_INFORMATION,
                             FALSE, 0)) {
            MC_WARN("[profiler] could not duplicate the guest thread handle; profiling is off");
            return;
        }

        g_pinned_tid = pinned_tid;
        g_pinned_name = target_name;
        g_targets.push_back({pinned, pinned_tid, target_name, true});

        // Warm the unwind-table lookup on this thread's own return address, so
        // the first call from the sampler is not the one doing any lazy setup
        // while a target is suspended.
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
                         "up to %d threads sampled at %d Hz each (%d Hz total ceiling), "
                         "report every %.0f s, slow frame >= %.0f ms\n"
                         "guest addresses come from the codegen function table, not from "
                         "symbols -- they are valid with no PDB present\n"
                         "and can be opened directly in the IDB.\n"
                         "call stacks are captured during slow frames only.\n"
                         "pinned: %s\n",
                         kMaxTargets, kPerTargetRateHz, kMaxTotalRateHz, kReportIntervalSec,
                         SlowFrameMs(), g_pinned_name.c_str());
            WriteMachineHeader(g_log);
            std::fflush(g_log);
        }

        g_running.store(true, std::memory_order_relaxed);
        g_sampler = std::thread(SamplerLoop);
        MC_INFO("[profiler] sampling up to {} threads at {} Hz -> {}", kMaxTargets,
                kPerTargetRateHz, name);
    }

    g_last_tick_ns.store(NowNs(), std::memory_order_relaxed);
    const bool slow = frame_ms >= SlowFrameMs();
    g_frame_is_slow.store(slow, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_frame_mtx);
    g_total_frames++;
    if (slow) g_total_slow_frames++;
}

void Report(const char* reason) {
    // Only meaningful once the sampler has stopped; while it runs, it owns the
    // sample buffers and reports on its own cadence.
    if (!Enabled() || !g_log || g_running.load(std::memory_order_relaxed)) return;
    double cpu_window = 0.0;
    std::vector<CpuRow> rows = CollectThreadCpu(&cpu_window);
    WriteReport(reason, cpu_window, rows, cpu_window);
}

void Shutdown() {
    if (!Enabled()) return;
    if (g_running.exchange(false, std::memory_order_relaxed)) {
        if (g_sampler.joinable()) g_sampler.join();
        if (!g_stall_dumped.load(std::memory_order_relaxed)) DumpAllThreads("shutdown");
        Report("final");
    }
    for (Target& t : g_targets) {
        if (t.h) CloseHandle(t.h);
    }
    g_targets.clear();
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
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
