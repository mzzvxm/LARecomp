#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Prop owners at the start of an online race.
//
// Every rage::fragInst the physics simulator creates asks the game for an
// "owner" through a functor the prop manager registered (sub_8272B218): the
// prop manager mallocs a 32-byte mcProp (vtable 0x820CC6C0, +16 = the inst)
// and the inst keeps it at +0x8C. Every update the simulator hands that owner
// back (functor at sim+0x34C, fn at sim+0x358 = sub_82728BB8), and the prop
// manager reads the owner's inst (+16) and its height (+68) to catch props
// that fell through the world.
//
// Starting an online race (System Link or Xbox LIVE, a Match or a challenge)
// resets the world (sub_822611B0 -> mcPropManager reset sub_82729D28 -> every
// prop manager's slot 3, mcPropManagerRaceData's being sub_8272C840), which
// re-syncs each race prop through that functor:
//
//   sub_82729D28 -> sub_82732AD8 -> sub_82735438 -> sub_8248AF18 ->
//   sub_8248CE08 -> [functor] sub_82728BB8: lwz r11,16(r4); lfs f13,68(r11)
//
// Measured 19/10/2026 on the Release build with the user's mods: inst
// B2D5EAA0 (mcPropInst) alive, its owner BEADF000 alive, and the owner's +16
// read 0DD5EAA0 -- four bytes at BEADF00D had become DE AD F0 0D the moment
// the match session was created. That is XAM's placeholder session object
// (0xDEADF00D, the same physical page as 0xBEADF00D) being written by the
// XSessionCreate handler; xbox_live.cpp stops it at the source.
//
// What stays here is a safety net: both places the simulator calls that
// functor (0x8248CE88 in sub_8248CE08, 0x8248C31C in sub_8248C228) skip the
// call when the owner or the owner's inst is not a readable guest pointer,
// and say what they found. Owners are tracked from creation (0x82728A30) to
// release (0x82728A98, the inst destructor's functor, with the guest call
// chain) so the report can tell a live owner from a freed one.
//
// online_race_prop_watch=<inst hex> polls that inst's owner word every frame
// and logs when it changes, with the memory around it. A hardware watchpoint
// would not have caught the write above: it came through another view of the
// same physical memory.
//
// Registered as midasm hooks in larecomp_config.toml.
//

#include "online_common.h"
#include "race_props.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(online_race_prop_guard, true, "MCLA/Multiplayer",
                    "Skip the physics update of a prop whose owner is broken instead of "
                    "crashing. Starting an online race re-syncs every race prop.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(online_race_prop_watch, "", "MCLA/Multiplayer",
                      "Diagnostic: guest address (hex) of a prop inst. Its owner's inst word "
                      "is polled every frame and every change is logged. Empty = off.");

namespace {

constexpr uint32_t kOwnerVtable = 0x820CC6C0;
constexpr uint32_t kOwnerInstAt = 16;
constexpr uint32_t kDetailedReports = 24;

struct FreedOwner {
    uint32_t inst = 0;
    std::array<uint32_t, 8> chain{};
};

std::mutex g_mu;
std::unordered_map<uint32_t, uint32_t> g_live;        // owner -> inst it was made for
std::unordered_map<uint32_t, FreedOwner> g_freed;     // owner -> how it went
std::atomic<uint32_t> g_bad{0};

rex::memory::Memory* Mem() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->memory() : nullptr;
}

bool Readable(rex::memory::Memory* mem, uint32_t addr, uint32_t len) {
    if (addr < 0x10000u || addr > 0xFFFFFFFFu - len) return false;
    auto* heap = mem->LookupHeap(addr);
    if (!heap) return false;
    const auto access = heap->QueryRangeAccess(addr, addr + len - 1);
    return (static_cast<uint32_t>(access) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) != 0;
}

// The heaps a live game object can sit in; anything below is not one.
bool LooksLikeObject(uint32_t addr) { return addr >= 0x40000000u && addr < 0xFFFF0000u; }

// Guest return addresses above the hook: r1 is the current function's frame
// (after its stwu); each frame's back chain is at +0 and the caller's LR was
// stored 8 bytes below the frame it points at.
std::array<uint32_t, 8> GuestChain(rex::memory::Memory* mem, uint32_t sp) {
    std::array<uint32_t, 8> out{};
    uint32_t fp = sp;
    for (size_t i = 0; i < out.size(); ++i) {
        if (!Readable(mem, fp, 4)) break;
        const uint32_t back = GuestRead32(mem, fp);
        if (back <= fp || !Readable(mem, back - 8, 4)) break;
        out[i] = GuestRead32(mem, back - 8);
        fp = back;
    }
    return out;
}

std::string ChainText(const std::array<uint32_t, 8>& chain) {
    std::string s;
    for (uint32_t lr : chain) {
        if (!lr) break;
        if (!s.empty()) s += " <- ";
        s += fmt::format("{:08X}", lr);
    }
    return s.empty() ? std::string("?") : s;
}

std::string HexDump(rex::memory::Memory* mem, uint32_t from, uint32_t len) {
    std::string out;
    for (uint32_t a = from; a < from + len; a += 16) {
        out += fmt::format("\n    {:08X}:", a);
        for (uint32_t k = 0; k < 16; k += 4) {
            out += Readable(mem, a + k, 4) ? fmt::format(" {:08X}", GuestRead32(mem, a + k)) : std::string(" ????????");
        }
    }
    return out;
}

// ── online_race_prop_watch ───────────────────────────────────────────────────
std::string g_watch_text;
uint32_t g_watch_inst = 0;
std::atomic<uint32_t> g_watch_word{0};   // owner + 16 of the watched inst, 0 = none
uint32_t g_watch_last = 0;
bool g_watch_last_valid = false;

uint32_t WatchInst() {
    const std::string text = REXCVAR_GET(online_race_prop_watch);
    if (text != g_watch_text) {
        g_watch_text = text;
        g_watch_inst = text.empty() ? 0u : static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
    }
    return g_watch_inst;
}

void PollWatch() {
    const uint32_t word = g_watch_word.load(std::memory_order_relaxed);
    if (!word) return;
    auto* mem = Mem();
    if (!mem || !Readable(mem, word, 4)) return;
    const uint32_t v = GuestRead32(mem, word);
    if (g_watch_last_valid && v == g_watch_last) return;
    if (g_watch_last_valid) {
        LARECOMP_APP_WARN("[race-props] watched {:08X} changed {:08X} -> {:08X}; around it:{}", word,
                          g_watch_last, v, HexDump(mem, (word & ~0xFu) - 0x40, 0xA0));
    }
    g_watch_last = v;
    g_watch_last_valid = true;
}

// True when the owner functor must not run for this inst.
bool OwnerBroken(uint32_t inst, uint32_t owner, const char* site) {
    auto* mem = Mem();
    if (!mem) return false;

    // Fast path, every prop update: both pointers in the object heaps.
    uint32_t owner_inst = 0;
    if (LooksLikeObject(owner)) {
        owner_inst = GuestRead32(mem, owner + kOwnerInstAt);
        if (LooksLikeObject(owner_inst)) return false;
    }
    if (Readable(mem, owner, 32) && Readable(mem, owner_inst, 0x50)) return false;

    const uint32_t n = g_bad.fetch_add(1) + 1;
    if (n <= kDetailedReports) {
        const uint32_t owner_vt = Readable(mem, owner, 4) ? GuestRead32(mem, owner) : 0;
        const uint32_t inst_vt = Readable(mem, inst, 4) ? GuestRead32(mem, inst) : 0;
        std::string state = "unknown (never seen created)";
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (auto it = g_live.find(owner); it != g_live.end()) {
                state = fmt::format("live, made for inst {:08X}", it->second);
            } else if (auto f = g_freed.find(owner); f != g_freed.end()) {
                state = fmt::format("FREED (was inst {:08X}), freed via {}", f->second.inst,
                                    ChainText(f->second.chain));
            }
        }
        LARECOMP_APP_WARN(
            "[race-props] {} broken owner #{}: inst {:08X} (vtable {:08X}) owner {:08X} "
            "(vtable {:08X}{}) owner+16 {:08X} -- {}{}; memory around the owner:{}",
            site, n, inst, inst_vt, owner, owner_vt, owner_vt == kOwnerVtable ? " = prop owner" : "",
            owner_inst, state,
            REXCVAR_GET(online_race_prop_guard) ? "; physics update skipped" : "",
            Readable(mem, owner, 4) ? HexDump(mem, (owner & ~0xFu) - 0x40, 0xA0) : std::string(" (unreadable)"));
    } else if ((n & (n - 1)) == 0) {
        LARECOMP_APP_WARN("[race-props] {} broken owners so far", n);
    }
    return REXCVAR_GET(online_race_prop_guard);
}

}  // namespace

void TickRaceProps() { PollWatch(); }

// sub_827289E8 just built an owner: r3 = owner, r31 = the inst it serves.
void Hook_PropOwnerCreated(PPCRegister& r3, PPCRegister& r31) {
    const uint32_t owner = r3.u32;
    if (!owner) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_live[owner] = r31.u32;
        g_freed.erase(owner);
    }
    const uint32_t watch = WatchInst();
    if (watch && r31.u32 == watch) {
        LARECOMP_APP_WARN("[race-props] owner {:08X} created for watched inst {:08X}", owner, watch);
        g_watch_last_valid = false;
        g_watch_word.store(owner + kOwnerInstAt, std::memory_order_relaxed);
    }
}

// sub_82728A68 is about to free an owner: r31 = owner, r1 = its frame.
void Hook_PropOwnerFreed(PPCRegister& r31, PPCRegister& r1) {
    const uint32_t owner = r31.u32;
    auto* mem = Mem();
    if (!owner || !mem) return;
    FreedOwner f;
    f.chain = GuestChain(mem, r1.u32);
    std::lock_guard<std::mutex> lock(g_mu);
    if (auto it = g_live.find(owner); it != g_live.end()) {
        f.inst = it->second;
        g_live.erase(it);
    }
    g_freed[owner] = f;
}

// sub_8248CE08, before `bctrl` to the per-update owner functor:
// r31 = inst, r4 = *(inst + 0x8C). True jumps past the call (0x8248CE90).
bool Patch_PropOwnerSyncGuard(PPCRegister& r31, PPCRegister& r4) {
    return OwnerBroken(r31.u32, r4.u32, "sync");
}

// sub_8248C228, same functor at its tail: r31 = inst, r4 = owner. True jumps
// past the call (0x8248C324).
bool Patch_PropOwnerPoseGuard(PPCRegister& r31, PPCRegister& r4) {
    return OwnerBroken(r31.u32, r4.u32, "pose");
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void TickRaceProps() {}
void Hook_PropOwnerCreated(PPCRegister& r3, PPCRegister& r31) {}
void Hook_PropOwnerFreed(PPCRegister& r31, PPCRegister& r1) {}
bool Patch_PropOwnerSyncGuard(PPCRegister& r31, PPCRegister& r4) { return false; }
bool Patch_PropOwnerPoseGuard(PPCRegister& r31, PPCRegister& r4) { return false; }

#endif  // REXGLUE_HAS_XEO3_TARGET
