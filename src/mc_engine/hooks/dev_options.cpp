// MCLA's dev command-line options, reactivated: <exe>/debug_options.txt and the
// MCLA/Performance cvars that map onto the same switches. The option table is
// ../debug_options_table.inc, written by tools/debug_options/gen_native_table.py.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <sys/mman.h>
#include <cstdio>
#endif

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

// =============================================================================
// Debug/dev command-line options (reactivated)
// -----------------------------------------------------------------------------
// MCLA retail keeps ~540 dev options (unlockall, timeofday, noshadows, money…)
// registered as dead stubs at 0x827A76E0..0x827B8598 that branch to the list
// registrar 0x821C06C8. Each stub owns a node; node+4 is the "value" slot: a
// guest char* to an ASCII value string. The stripped retail parser never fills
// it, so every consumer's guard (`if (node+4) …`) is false and the getter
// sub_821C0750 (which derefs node+4 and atois it) is never reached.
//
// Consumers read node+4 exactly once, at init/level-load (verified in IDA:
// timeofday -> mcLightingManager ctor sub_822F3498; money -> profile
// deserializer sub_826BACF0; showframerate -> render init sub_822D68F8). An
// external process that writes after boot is always too late — which is why
// post-boot injection changed nothing. InitHooks runs at startup, before those
// consumers, so populating node+4 here is equivalent to the dev command line.
//
// A pointer to "1" satisfies both consumer shapes: bool guards see nonzero, and
// the atoi getter parses the string. Value strings are parked in the dead stub
// region itself (mapped, never executed after the strip).
struct DebugOption {
    const char* name;
    uint32_t value_addr;  // guest address of node+4
};
#include "../debug_options_table.inc"

// Dead registration-stub bytes double as scratch for the value strings.
static constexpr uint32_t kDbgScratchStart = 0x827A76E0u;
static constexpr uint32_t kDbgScratchEnd   = 0x827B8500u;

static const DebugOption* FindDebugOption(const std::string& name) {
    for (const auto& opt : kDebugOptions)
        if (name == opt.name) return &opt;
    return nullptr;
}

// Bump allocator over the dead stub region, shared by every writer of an option
// value so two callers never hand out the same bytes.
static uint32_t g_dbg_scratch_cursor = kDbgScratchStart;

// The stub region is part of the XEX code image, so it is mapped read+execute
// and writing a value string into it faults (guest AV at 0x827A76E0). Nothing
// ever executes from guest memory in a recomp — the code is native — so making
// these pages writable costs nothing. Done once, lazily, and a failure disables
// the whole mechanism instead of crashing.
static bool EnsureDebugScratchWritable() {
    enum class State { kUnknown, kOk, kFailed };
    static State state = State::kUnknown;
    if (state != State::kUnknown) return state == State::kOk;
    state = State::kFailed;

    constexpr uint32_t kPage = 0x1000u;
    const uint32_t lo = kDbgScratchStart & ~(kPage - 1u);
    const uint32_t hi = (kDbgScratchEnd + kPage - 1u) & ~(kPage - 1u);
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;

    // Reports whether the host pages backing the range really are writable now.
    // BaseHeap::Protect can return true having applied something else entirely
    // (it takes rex::memory::kMemoryProtect* flags, not the X_PAGE_* family —
    // handing it an X_PAGE_ value silently maps to kNoAccess), so the return
    // value is not evidence on its own.
    auto range_is_writable = [&]() -> bool {
#if defined(_WIN32)
        for (uint32_t a = lo; a < hi;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) return false;
            constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State != MEM_COMMIT || !(mbi.Protect & kWritable)) return false;
            const uint64_t end =
                uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
            if (end <= a) return false;
            a = uint32_t(end);
        }
        return true;
#else
        // The same check from /proc/self/maps: every byte of the range must lie
        // in a mapping whose permissions include 'w'.
        std::ifstream maps("/proc/self/maps");
        uintptr_t want = reinterpret_cast<uintptr_t>(base + lo);
        const uintptr_t want_end = reinterpret_cast<uintptr_t>(base + hi);
        std::string line;
        while (want < want_end && std::getline(maps, line)) {
            unsigned long long start = 0, end = 0;
            char perms[5] = {};
            if (std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms) != 3) continue;
            if (end <= want || start > want) continue;
            if (perms[1] != 'w') return false;
            want = uintptr_t(end);
        }
        return want >= want_end;
#endif
    };

    auto* memory = rex::Runtime::instance()->memory();
    auto* heap = memory ? memory->LookupHeap(kDbgScratchStart) : nullptr;
    if (heap) {
        constexpr uint32_t kRW =
            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        if (heap->Protect(lo, hi - lo, kRW) && range_is_writable()) {
            state = State::kOk;
            LARECOMP_APP_INFO("[DbgOpt] scratch 0x{:08X}-0x{:08X} writable (guest heap)", lo, hi);
            return true;
        }
    }

#if defined(_WIN32)
    // The image is not always tracked by a heap whose Protect reaches the host
    // mapping. The arena is an ordinary host reservation, so protect it directly.
    DWORD old = 0;
    if (VirtualProtect(base + lo, hi - lo, PAGE_EXECUTE_READWRITE, &old) &&
        range_is_writable()) {
        state = State::kOk;
        LARECOMP_APP_INFO("[DbgOpt] scratch 0x{:08X}-0x{:08X} writable (host, was 0x{:X})",
                          lo, hi, uint32_t(old));
        return true;
    }
#else
    // Same as above: the arena is an ordinary host mapping, so protect it directly.
    if (mprotect(base + lo, hi - lo, PROT_READ | PROT_WRITE) == 0 && range_is_writable()) {
        state = State::kOk;
        LARECOMP_APP_INFO("[DbgOpt] scratch 0x{:08X}-0x{:08X} writable (host)", lo, hi);
        return true;
    }
#endif

    LARECOMP_APP_ERROR("[DbgOpt] cannot make scratch 0x{:08X}-0x{:08X} writable; "
                       "dev options disabled", lo, hi);
    return false;
}

// Parks `value` in guest scratch and points node+4 at it — exactly the state the
// dev command line would have left. Returns false if the scratch region is
// exhausted or could not be made writable.
static bool SetDebugOptionValue(uint32_t value_addr, const std::string& value) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) {
        LARECOMP_APP_ERROR("[DbgOpt] no guest membase");
        return false;
    }
    if (!EnsureDebugScratchWritable()) return false;

    uint32_t need = static_cast<uint32_t>(value.size()) + 1;
    if (g_dbg_scratch_cursor + need > kDbgScratchEnd) {
        LARECOMP_APP_ERROR("[DbgOpt] scratch full, dropping node+4 @ 0x{:08X}", value_addr);
        return false;
    }

    const uint32_t at = g_dbg_scratch_cursor;
    for (size_t i = 0; i < value.size(); ++i)
        base[at + i] = static_cast<uint8_t>(value[i]);
    base[at + value.size()] = 0;

    // node+4 = big-endian guest pointer to that string.
    base[value_addr + 0] = static_cast<uint8_t>(at >> 24);
    base[value_addr + 1] = static_cast<uint8_t>(at >> 16);
    base[value_addr + 2] = static_cast<uint8_t>(at >> 8);
    base[value_addr + 3] = static_cast<uint8_t>(at);

    g_dbg_scratch_cursor += (need + 3u) & ~3u;
    return true;
}

// Writing node+4 from InitHooks does not survive. The registration stubs are
// ordinary guest static initializers: they run when the guest starts, which is
// after InitHooks, and the registrar sub_821C06C8 clears the slot itself —
//
//   821C06CC  stw  r4, 8(r3)        node+8   = 0
//   821C06D4  stw  r5, 0(r3)        node+0   = name
//   821C06D8  stb  r6, 0x10(r3)     node+0x10= 0
//   821C06DC  stw  r9, 4(r3)        node+4   = 0     <- wipes an early write
//   821C06E0  lwz  r11, 0x59E4(r10) list head
//   821C06E4  stw  r11, 0xC(r3)     node+0xC = next
//   821C06E8  stw  r3, 0x59E4(r10)  head     = node
//   821C06EC  blr
//
// So the requests are only collected at InitHooks time and applied from
// Patch_DevOptionRegistered, hooked on that blr with r3 still holding the node.
// That is after the game has finished building the node and before any consumer
// can read it, for every option, which is the only point where both hold.

struct PendingDebugOption {
    uint32_t value_addr;  // node+4
    std::string value;
};

static std::vector<PendingDebugOption> g_pending_options;

static void QueueDebugOption(const std::string& name, const std::string& value) {
    const DebugOption* opt = FindDebugOption(name);
    if (!opt) {
        LARECOMP_APP_ERROR("[DbgOpt] unknown option '{}'", name);
        return;
    }
    const std::string v = value.empty() ? std::string("1") : value;
    for (auto& p : g_pending_options) {
        if (p.value_addr == opt->value_addr) {  // last writer wins
            p.value = v;
            return;
        }
    }
    g_pending_options.push_back({opt->value_addr, v});
    LARECOMP_APP_INFO("[DbgOpt] queued {} = {} (node+4 @ 0x{:08X})", name, v, opt->value_addr);
}

// The MCLA/Performance cvars backed by a dev switch. perf_no_shadows is NOT
// here: its only effect is a mask edit the renderer does once, at construction,
// so it is applied directly and per-frame in ApplyRenderPhaseMask() instead —
// which also makes it take hold without a restart.
struct PerfDebugOption {
    const char* cvar;
    const char* option;
};

static constexpr PerfDebugOption kPerfDebugOptions[] = {
    {"perf_no_race_shadows",      "noraceshadows"},
    {"perf_fast_vehicle_shadows", "fastVehShadows"},
    {"perf_no_impostors",         "noimpostors"},
    {"perf_no_trees",             "notrees"},
    {"perf_no_fullscreen_blur",   "nofsblur"},
};

void ApplyPerfDebugOptions() {
    for (const auto& p : kPerfDebugOptions) {
        if (rex::cvar::GetFlagByName(p.cvar) != "true") continue;
        QueueDebugOption(p.option, "1");
    }
}

// These switches are load-time, not per-frame state: noimpostors skips creating
// the ImpostorDepth/ShadowImpostor/ImpostorColor/ImpostorNormal render targets
// in sub_8230B778, and notrees skips the whole prop parse in sub_82310478. The
// game has no path to build or tear those down while running, so they cannot be
// toggled instantly.
//
// What this does buy: node+4 is kept in sync with the cvar every frame, so the
// consumers pick the new value up the next time their system loads — a district
// change or a world reload — instead of needing the process restarted.
//
// The scratch pointer for "1" is allocated once per option and reused, so
// flipping a switch repeatedly does not walk the bump allocator forward.
void ApplyLoadTimeDevOptions() {
    static bool primed = false;
    static bool last[std::size(kPerfDebugOptions)] = {};
    static uint32_t value_ptr[std::size(kPerfDebugOptions)] = {};

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    for (size_t i = 0; i < std::size(kPerfDebugOptions); ++i) {
        const auto& p = kPerfDebugOptions[i];
        const bool want = rex::cvar::GetFlagByName(p.cvar) == "true";
        if (primed && want == last[i]) continue;
        last[i] = want;

        const DebugOption* opt = FindDebugOption(p.option);
        if (!opt) continue;

        if (!want) {
            WriteGuestU32(base, opt->value_addr, 0);  // exactly what the registrar leaves
        } else {
            if (value_ptr[i] == 0) {
                if (!SetDebugOptionValue(opt->value_addr, "1")) continue;
                value_ptr[i] = ReadGuestU32(base, opt->value_addr);
            } else {
                WriteGuestU32(base, opt->value_addr, value_ptr[i]);
            }
        }
        if (primed) {
            LARECOMP_APP_INFO("[DbgOpt] {} -> {} (applies on next load of that system)",
                              p.option, want ? "on" : "off");
        }
    }
    primed = true;
}

// Reads <exe dir>/debug_options.txt: one `name` or `name=value` per line,
// '#'/';' comments and blank lines ignored. Bare name means value "1".
void ApplyDebugOptions() {
    std::error_code ec;
    std::filesystem::path file = std::filesystem::current_path(ec) / "debug_options.txt";
    if (ec || !std::filesystem::exists(file, ec)) return;

    std::ifstream in(file);
    if (!in) {
        LARECOMP_APP_ERROR("[DbgOpt] cannot open {}", file.string());
        return;
    }

    int applied = 0, unknown = 0;
    std::string line;
    while (std::getline(in, line)) {
        // strip whitespace + inline comments
        auto cut = line.find_first_of("#;");
        if (cut != std::string::npos) line.erase(cut);
        auto b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        std::string name = line, value = "1";
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            name = line.substr(0, eq);
            value = line.substr(eq + 1);
            auto nb = name.find_last_not_of(" \t");
            if (nb != std::string::npos) name.erase(nb + 1);
            auto vb = value.find_first_not_of(" \t");
            value = (vb == std::string::npos) ? std::string() : value.substr(vb);
        }
        if (name.empty()) continue;

        if (!FindDebugOption(name)) {
            LARECOMP_APP_ERROR("[DbgOpt] unknown option '{}'", name);
            ++unknown;
            continue;
        }
        QueueDebugOption(name, value);
        ++applied;
    }
    LARECOMP_APP_INFO("[DbgOpt] queued {} option(s) from file, {} unknown", applied, unknown);
}

// 0x821C06EC, the blr of the dev-option registrar sub_821C06C8. r3 still holds
// the node the game just finished building, and the instruction right before us
// (`stw r9, 4(r3)`) has already cleared node+4 — so this is the first and only
// moment a value can be planted where nothing will wipe it and every consumer
// still reads it later.
void Patch_DevOptionRegistered(PPCRegister& r3) {
    if (g_pending_options.empty()) return;

    const uint32_t value_addr = static_cast<uint32_t>(r3.u64) + 4;
    for (auto it = g_pending_options.begin(); it != g_pending_options.end(); ++it) {
        if (it->value_addr != value_addr) continue;
        if (SetDebugOptionValue(value_addr, it->value)) {
            LARECOMP_APP_INFO("[DbgOpt] applied node+4 @ 0x{:08X} = {}", value_addr, it->value);
        }
        g_pending_options.erase(it);
        return;
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void ApplyDebugOptions() {}
void Patch_DevOptionRegistered(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
