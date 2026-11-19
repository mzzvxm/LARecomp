// DEBUG ONLY: the traffic probe (TRAFFIC-PROBE, perf\TRAFFIC-SCRIPT.md section 5.4 step 1). It READS the game's ambient (traffic), police and
// opponent state and logs it, and, from a second cvar of its own, makes ONE call that asks the game to place an ambient car. Both are off
// by default and never saved (.debug_only()).
//
// 1. mcla_traffic_probe N: every N updates the log gets the `[traffic-probe]` lines of a sample: the player's pose and the density chain,
//    the ambient manager's pool and the layout's pool (capacity, live count, free slots), every live ambient car (model, position, heading,
//    speed, the rail it is on, its LOD), the opponent and racer pools, the police manager's patrols, and one `SUMMARY` line a chain can
//    read. Nothing is written into the guest: every access is a read, behind a readable-memory check, walks are bounded.
// 2. mcla_traffic_probe_spawn S: a set to a non-zero value that differs from the last acted on calls AmbientMgr_AddAmbientToLayout ONCE
//    (0x826D6850, through the toolkit's native call shim) with the model of mcla_traffic_probe_model (empty: the first regular car type of
//    the game's list), `mcla_traffic_probe_ahead` metres in front of the player. Guards (traffic_probe_core.h DecideSpawn): free roam only
//    (the game's Cruising state), a car being driven (not paused), the world up for 120 updates, a layout pool slot free (two to spare), the
//    model in the game's type list, a plausible spawn point, at most 3 calls in a run of the game. A refused set is logged with its reason and
//    used up. For 120 s after a call the log follows the new layout item and the ambient cars (any class) within 8 m of the spawn point.
//
// What AmbientMgr_AddAmbientToLayout MAKES (read in the recompiled game; the arguments are a model NAME string (a type of the manager's list; the native
// copies it with snprintf("%s") into a 32-byte buffer), a position (a pointer to three floats) and a heading in DEGREES (stored as radians)):
//   it takes a slot from the layout pool's class 0 free list (sub_826E0C40; an empty pool: it logs "[AMBIENT LAYOUT] AddAmbientToLayout failed" and returns -1), fills
//   the slot (name +40, position +96, +132 heading, +128 = 0 (no path data), +140 = 1), links it into the pool's 'used' list, calls the slot's vtable entry 4, and returns the
//   slot's id (+136) [V sub_826D6850, sub_826E1418]. It makes NO car by itself: a layout item is a PLACEMENT. The consumer is the PARKED spawner (sub_826F23A0, in the zone step
//   next to the traffic and pedestrian spawners; it needs the enable byte 0x8282A7CA and a byte at mgr+0x642C7): it takes a layout item whose +76 is 0 and +80 is negative (not
//   bound to a car yet), finds the type by the item's NAME in the manager's type list (sub_826D5390) and calls sub_82687A60, which builds a spawn ticket (sub_826DF430) and
//   binds it to the item (+76, +80) [V]. With +128 = 0 the car has no authored path, so the expectation is a PARKED car at the placement ([I]); the trigger path
//   (Trigger_AddEventAmbient, sub_82687F58) is what drives a layout car along a path. Whether a runtime-added item is found by the parked spawner's pick (it walks the layout
//   the zone's cell lists name) is CHECK: the follow-up of a spawn says (does an ambient appear, in which class, does it move, does it go).
//   AmbientMgr_SetTrafficEnable(false) (sub_826D67F0) stores 0 in 0x8282A7C8 and sends the despawn message 0x60 (sub_826D56F0 now, or queued when [mgr+0x64259] is set), so it
//   gates new traffic spawns (sub_826F6920 and its candidate test sub_826EE018 return at once when it is 0) and removes the ambients the message names [V]; the traffic spawner picks a rail
//   of the railnet (survey), and was not seen to read the layout.
//
// What is read and how sure that reading is ([V] read in the recompiled game this session, tree-t2 generated code; [I] inferred; CHECK = a
// run has to show it). Guest addresses are the game's.
//
//   Ambient manager  [0x8286D8E4] (mgr)                                                          [V]
//     +4        the type list: [mgr+4] -> [+0] the first type; a type's name pointer is at +172 and its next link at +20 -> [+0]   [V sub_826D5390]
//     +12       the live ambients' pool: an array of 52-byte descriptors, one per class                                           [V sub_826DAFF8, sub_826D6290]
//               descriptor +0 free list head, +8 live list head, +24 pending list head, +32 used list head; the counts at +16 / +20 / +40 / +44
//               (free / live / pending / used); a list node is {item, next at +4, prev at +8}                                 [V sub_826E0C40, sub_826D6390]
//               class 0 = moving traffic cars (the unspawn pass picks its candidates from it), 1 = parked, 2 = pedestrians       [I sub_826DE670 / sub_826D6B48]
//     +8556     the car types: array pointer at [+0], weight array at [+8], u16 count at mgr+8568; a type's group is +116, +113 marks the
//               special ones the random pick leaves out                                                                           [V sub_826DA638]
//     +410144   the layout object (0x64220): [layout+0] -> the layout data, whose +32 is the layout's pool (descriptors as above)     [V sub_826E1418]
//     +225904   the 31 zones (5936 bytes each); zone 0: +960 the density factor, +968 the player's speed, +972 the player slot, +1796 / +1800
//               the two density sums, +2122 the zone-enabled byte                                                                 [V survey + sub_826F79F0]
//   A layout item (what AddAmbientToLayout fills): +40 the model name (32 bytes), +96 the position, +132 the heading (radians), +136 its id
//     (what the native returns), +140 a byte (1); +76 / +80 are read by the trigger path (sub_82687F58: +76 a bound object, +80 a state,
//     negative = not yet used) [V the stores; I the meaning]
//   A live ambient (class 0)
//     +128..191 the update copy of its matrix (rows right, up, back, position; the position is the row at +48)                   [V gpu/traffic_shadow.h]
//     +500 flags (bit 0x20 = protected from the unspawn pass), +480 the zone's player slot, +40 non-zero = placed by a layout item (the unspawn
//     pass skips it), +452 -> the navigation object, whose +188 -> the RAIL the car is on (rail +4 flags, +8 the parent road, +164 the per-rail
//     density AmbientMgr_SetRoadDensity writes; the road's +148 the speed limit)                                                 [V sub_826E9870, sub_826DCCA0, sub_822447B8]
//     +448 the car's ambient type (sub_826DE670 stores the spawn's type there), whose +172 is the model name                        [I: one store; CHECK in the log]
//     the speed is derived from two samples' positions (no velocity field was found); the LOD is the render item's (+28), seen by the draw hook
//     TrafficProbeNoteItem (traffic_shadow.cpp calls it) and matched by the item's car pointer (+16)                                [V gpu/traffic_shadow.h]
//   Opponent manager [0x8286EBE4] (M): +8 the active array, u16 count at +12; +24 the racers array, u16 count at +28; +32 the free pool P:
//     u16 free count at P+6, the free list head at [P+12] (node[0] = next)                                                        [V sub_822B8820, sub_822B81F8]
//   Racer manager    [0x828745E4]: an array of up to 100 racers at +0, the i32 count at +400                                        [V sub_822B61B0; the 100 is [I]]
//   Police manager   [0x8286D8E0] (pm): the update thread's array at [pm+4] with the u16 count at pm+8 (the render thread's copy: pm+12, u16 at
//     pm+16); a police car: +872 alive, +1072 active, +1073 allowed to patrol, +1152 non-zero = not counted                        [V sub_82701080, sub_827010E8,
//     sub_82703240 (SetAllPoliceActive(b) sets +1072 and +1073 of every police with +872 set), sub_82708F30 (a police with +1072 clear is not stepped)]
//   The enable bytes 0x8282A7C8 / 9 / A (traffic / peds / parked): AmbientMgr_SetTrafficEnable(b) stores the first, and for b = 0 sends the
//     despawn message 0x60 (queued when [mgr+0x64259] is set, otherwise applied at once by sub_826D56F0); SetAllPoliceActive(false) is as above. [V]
//
// Threads: the tick runs on the game thread (Patch_DeltaTimePre). The ambient task thread changes the same lists, so a walk can see a list
// in the middle of a change: it is bounded by a count and by readable-memory checks, and an odd line is only a diagnostic.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#endif

#include "discord_rpc/discord_rpc.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "mc_engine/cinematic/native_call.h"
#include "mc_engine/input/adaptive_triggers.h"
#include "traffic_probe_core.h"

REXCVAR_DECLARE(double, traffic_density_scale);  // world_tuning.cpp
REXCVAR_DECLARE(double, parked_car_scale);       // world_tuning.cpp

REXCVAR_DEFINE_INT32(
    mcla_traffic_probe, 0, "MCLA/Diagnostics",
    "Debug, live, never saved: every this many updates log the ambient (traffic) cars one by one (model, position, heading, speed, the rail, "
    "the LOD), the ambient and layout pools (capacity, live count, free slots), the opponent and racer pools and the police patrols, and one "
    "SUMMARY line, all as '[traffic-probe]' lines. It reads the game and changes nothing. 0 = off.")
    .debug_only()
    .range(0, 36000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_traffic_probe_detail, 1, "MCLA/Diagnostics",
    "Debug: how much of each traffic probe sample is logged. 0 = the pools, the census and the SUMMARY line; 1 = and the 20 nearest cars; 2 = "
    "and every car (up to 200). The census always counts all of them.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_traffic_probe_spawn, 0, "MCLA/Diagnostics",
    "Debug, live, never saved: a set to a number other than 0 (and other than the last one acted on) makes ONE call to the game's "
    "AmbientMgr_AddAmbientToLayout, asking it to place an ambient car of mcla_traffic_probe_model mcla_traffic_probe_ahead metres in front "
    "of the player, and follows what comes of it in the log for two minutes. Free roam only, with a layout slot free, at most three calls "
    "in a run of the game; a refusal is logged with its reason. 0 = off and re-arms (the same number again does nothing).")
    .debug_only()
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_traffic_probe_model, "", "MCLA/Diagnostics",
    "Debug: the ambient type (the car's model name in the game's own list, logged by the traffic probe as 'types') the spawn test asks for. "
    "Empty = the first regular car type of the game's list.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_traffic_probe_ahead, 25.0, "MCLA/Diagnostics",
    "Debug: how far in front of the player's car, in metres, the spawn test places the car.")
    .debug_only()
    .range(5.0, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using namespace mcla::traffic_probe;

// ---------------------------------------------------------------- guest addresses (see the header comment)

constexpr uint32_t kAmbientMgrPtr = 0x8286D8E4u;
constexpr uint32_t kMgrTypeList = 4u;
constexpr uint32_t kMgrPool = 12u;
constexpr uint32_t kMgrCarTypes = 8556u;       // the container: [+0] array, [+8] weights, u16 [+12] count
constexpr uint32_t kMgrCarTypeCount = 8568u;   // u16 (= 8556 + 12)
constexpr uint32_t kMgrLayout = 410144u;       // 0x64220
constexpr uint32_t kMgrZones = 225904u;
constexpr uint32_t kMgrParkedLayoutByte = 410311u;  // 0x642C7: a byte the parked spawner (sub_826F23A0) wants non-zero before it picks a layout item [V]
constexpr uint32_t kZoneFactor = 960u;
constexpr uint32_t kZoneSpeed = 968u;
constexpr uint32_t kZoneSumA = 1796u;
constexpr uint32_t kZoneSumB = 1800u;
constexpr uint32_t kZoneEnabled = 2122u;
constexpr uint32_t kTrafficEnableByte = 0x8282A7C8u;
constexpr uint32_t kPedEnableByte = 0x8282A7C9u;
constexpr uint32_t kParkedEnableByte = 0x8282A7CAu;
constexpr uint32_t kPlayerVehiclePtr = 0x8288DCF8u;
constexpr uint32_t kOpponentMgrPtr = 0x8286EBE4u;
constexpr uint32_t kRacerMgrPtr = 0x828745E4u;
constexpr uint32_t kPoliceMgrPtr = 0x8286D8E0u;
constexpr uint32_t kAddAmbientToLayout = 0x826D6850u;  // AmbientMgr_AddAmbientToLayout (natives.tsv)

constexpr uint32_t kDescBytes = 52u;
constexpr uint32_t kDescFreeHead = 0u, kDescLiveHead = 8u, kDescPendingHead = 24u, kDescUsedHead = 32u;
constexpr uint32_t kDescFree = 16u, kDescLive = 20u, kDescPending = 40u, kDescUsed = 44u, kDescRaw = 48u;
constexpr uint32_t kClasses = 3u;

constexpr uint32_t kTypeGroup = 116u, kTypeSpecial = 113u, kTypeName = 172u, kTypeNext = 20u;
constexpr uint32_t kObjMatrix = 128u, kObjFlags = 500u, kObjZone = 480u, kObjPlaced = 40u, kObjType = 448u, kObjNav = 452u, kObjBytes = 512u;
constexpr uint32_t kNavRail = 188u;
constexpr uint32_t kRailFlags = 4u, kRailParent = 8u, kRailDensity = 164u, kRoadLimit = 148u;
constexpr uint32_t kItemName = 40u, kItemBound = 76u, kItemState = 80u, kItemPos = 96u, kItemHeading = 132u, kItemId = 136u, kItemByte = 140u,
                   kItemBytes = 144u;
constexpr uint32_t kPoliceAlive = 872u, kPoliceActive = 1072u, kPoliceAllow = 1073u, kPoliceFlag = 1152u, kPoliceBytes = 1156u;

constexpr uint32_t kListCap = 600;       // nodes walked in one list, at the most
constexpr uint32_t kCarCap = 400;        // ambient cars read in one sample, at the most
constexpr uint32_t kNearestShown = 20;   // detail 1
constexpr uint32_t kAllShown = 200;      // detail 2
constexpr uint32_t kTypeCap = 512;
constexpr uint32_t kPoliceShown = 12;
constexpr double kNearSpawnM = 8.0;
constexpr double kFollowSeconds = 120.0;
constexpr uint32_t kFollowEvery = 30;    // updates between follow-up lines when the probe's own interval is off

// ---------------------------------------------------------------- guest memory

struct Guest {
    const uint8_t* base = nullptr;

    // Is [ea, ea + len) committed host memory (readable)? A pointer a stale list gave may well not be. (cinematic/cars.cpp's test.)
    bool Mapped(uint32_t ea, uint32_t len) const {
        if (!base || ea < 0x10000u || len == 0 || uint64_t(ea) + len > 0x100000000ull) return false;
#if defined(_WIN32)
        constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
        uint64_t at = ea;
        const uint64_t end = uint64_t(ea) + len;
        while (at < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + at, &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kRead) == 0) return false;
            const uint64_t region_end = uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
            if (region_end <= at) return false;
            at = region_end;
        }
        return true;
#else
        constexpr uint64_t kPage = 0x1000;
        const uint64_t end = uint64_t(ea) + len;
        for (uint64_t page = uint64_t(ea) & ~(kPage - 1); page < end; page += kPage) {
            const uint64_t at = std::max<uint64_t>(page, ea);
            uint8_t probe = 0;
            iovec local{&probe, 1};
            iovec remote{const_cast<uint8_t*>(base) + at, 1};
            const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
            if (n < 0 && (errno == ENOSYS || errno == EPERM)) return ea < 0xC0000000u;
            if (n != 1) return false;
        }
        return true;
#endif
    }

    // Unchecked reads (the caller has checked Mapped for the span).
    uint32_t U32(uint32_t ea) const { return ReadGuestU32(base, ea); }
    uint16_t U16(uint32_t ea) const { return uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]); }
    uint8_t U8(uint32_t ea) const { return base[ea]; }
    float F32(uint32_t ea) const { return ReadGuestF32(base, ea); }
    int32_t S32(uint32_t ea) const { return int32_t(U32(ea)); }

    // Checked: false when the word is not readable.
    bool Get32(uint32_t ea, uint32_t* v) const {
        if (!Mapped(ea, 4)) return false;
        *v = U32(ea);
        return true;
    }
};

bool Plausible(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }
bool PlausibleF(float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; }

// A C string in the guest at `ptr` (up to `max` chars), "" when it is not a name.
std::string ReadName(const Guest& g, uint32_t ptr, size_t max = 48) {
    if (!Plausible(ptr)) return std::string();
    for (size_t span = max; span >= 8; span /= 2) {  // a name at the end of a mapped region: a shorter span
        if (g.Mapped(ptr, uint32_t(span))) return PrintableName(g.base + ptr, span);
    }
    return std::string();
}

double NowSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- the render item's LOD, seen by the draw hook

struct NoteSlot {
    std::atomic<uint32_t> car{0};
    std::atomic<int32_t> lod{0};
    std::atomic<uint32_t> depth_bits{0};
    std::atomic<uint32_t> enabled{0};
    std::atomic<uint64_t> frame{0};
};
constexpr size_t kNoteSlots = 1024;
constexpr uint32_t kNoteProbes = 4;
NoteSlot g_notes[kNoteSlots];
std::atomic<bool> g_note_on{false};
std::atomic<const uint8_t*> g_note_base{nullptr};
std::atomic<uint64_t> g_note_frame{0};

size_t NoteHash(uint32_t car) { return size_t((car * 2654435761u) >> 22) & (kNoteSlots - 1); }

struct LodSeen {
    bool seen = false;
    int32_t lod = 0;
    float depth = 0.0f;
    bool enabled = false;
    uint64_t age = 0;  // updates since the draw last saw it
};

LodSeen FindNote(uint32_t car, uint64_t now_frame) {
    LodSeen out;
    const size_t h = NoteHash(car);
    for (uint32_t i = 0; i < kNoteProbes; ++i) {
        NoteSlot& s = g_notes[(h + i) & (kNoteSlots - 1)];
        if (s.car.load(std::memory_order_relaxed) != car) continue;
        out.seen = true;
        out.lod = s.lod.load(std::memory_order_relaxed);
        out.depth = std::bit_cast<float>(s.depth_bits.load(std::memory_order_relaxed));
        out.enabled = s.enabled.load(std::memory_order_relaxed) != 0;
        const uint64_t f = s.frame.load(std::memory_order_relaxed);
        out.age = now_frame >= f ? now_frame - f : 0;
        return out;
    }
    return out;
}

// How many different cars the draw hook has seen within `max_age` updates: a census that does not depend on the pool walk (if the walk finds
// nothing and this finds cars, the pool reading is wrong), and how many of them are drawn at all (a LOD of 0 or more).
void CountNotes(uint64_t now_frame, uint64_t max_age, uint32_t* seen, uint32_t* inview) {
    *seen = *inview = 0;
    for (const NoteSlot& s : g_notes) {
        if (s.car.load(std::memory_order_relaxed) == 0) continue;
        const uint64_t f = s.frame.load(std::memory_order_relaxed);
        if (now_frame < f || now_frame - f > max_age) continue;
        ++*seen;
        if (s.lod.load(std::memory_order_relaxed) >= 0 && s.enabled.load(std::memory_order_relaxed) != 0) ++*inview;
    }
}

// ---------------------------------------------------------------- pools

PoolCounts ReadDesc(const Guest& g, uint32_t pool, uint32_t cls) {
    PoolCounts c;
    const uint32_t d = pool + cls * kDescBytes;
    if (!Plausible(pool) || !g.Mapped(d, kDescBytes)) return c;
    c.free = g.U32(d + kDescFree);
    c.live = g.U32(d + kDescLive);
    c.pending = g.U32(d + kDescPending);
    c.used = g.U32(d + kDescUsed);
    c.raw48 = g.U32(d + kDescRaw);
    c.read = true;
    return c;
}

std::string DescText(const PoolCounts& c) {
    if (!c.read) return "unreadable";
    char b[160];
    if (!c.Sane()) {
        std::snprintf(b, sizeof(b), "NOT SANE (free %u live %u pend %u used %u +48 0x%08X)", c.free, c.live, c.pending, c.used, c.raw48);
    } else {
        std::snprintf(b, sizeof(b), "free %u live %u pend %u used %u (sum %u, +48 %u)", c.free, c.live, c.pending, c.used, c.Total(), c.raw48);
    }
    return b;
}

// The items of one list of a descriptor: {item, node} for each node, up to `cap`. Returns the nodes seen.
template <class F>
uint32_t WalkList(const Guest& g, uint32_t head_node, uint32_t cap, F&& fn) {
    uint32_t n = 0, node = head_node;
    while (node != 0 && n < cap) {
        if (!Plausible(node) || !g.Mapped(node, 12)) break;
        fn(g.U32(node), node);
        node = g.U32(node + 4);
        ++n;
    }
    return n;
}

struct ManagerView {
    uint32_t mgr = 0;
    uint32_t pool = 0;         // the live ambients' pool: descriptors
    uint32_t layout = 0;       // mgr + 0x64220
    uint32_t layout_data = 0;  // [layout]
    uint32_t layout_pool = 0;  // [layout_data + 32]
    bool Ok() const { return mgr != 0; }
};

ManagerView ReadManager(const Guest& g) {
    ManagerView m;
    uint32_t mgr = 0;
    if (!g.Get32(kAmbientMgrPtr, &mgr) || !Plausible(mgr) || !g.Mapped(mgr, 16)) return m;
    m.mgr = mgr;
    g.Get32(mgr + kMgrPool, &m.pool);
    m.layout = mgr + kMgrLayout;
    if (g.Get32(m.layout, &m.layout_data) && Plausible(m.layout_data)) g.Get32(m.layout_data + 32, &m.layout_pool);
    if (!Plausible(m.pool)) m.pool = 0;
    if (!Plausible(m.layout_pool)) m.layout_pool = 0;
    return m;
}

// ---------------------------------------------------------------- the type list

struct TypeInfo {
    uint32_t addr = 0;
    std::string name;
    int32_t group = 0;
    uint8_t special = 0;
};

std::vector<TypeInfo> ReadCarTypes(const Guest& g, uint32_t mgr) {
    std::vector<TypeInfo> out;
    uint32_t arr = 0;
    if (!g.Get32(mgr + kMgrCarTypes, &arr) || !Plausible(arr) || !g.Mapped(mgr + kMgrCarTypeCount, 2)) return out;
    const uint32_t count = std::min<uint32_t>(g.U16(mgr + kMgrCarTypeCount), kTypeCap);
    if (count == 0 || !g.Mapped(arr, 4 * count)) return out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t t = g.U32(arr + 4 * i);
        if (!Plausible(t) || !g.Mapped(t, kTypeName + 4)) continue;
        TypeInfo ti;
        ti.addr = t;
        ti.name = ReadName(g, g.U32(t + kTypeName));
        ti.group = g.S32(t + kTypeGroup);
        ti.special = g.U8(t + kTypeSpecial);
        out.push_back(std::move(ti));
    }
    return out;
}

// The manager's whole type list (sub_826D5390's walk): the names the layout's items are resolved against.
std::vector<TypeInfo> ReadTypeList(const Guest& g, uint32_t mgr) {
    std::vector<TypeInfo> out;
    uint32_t head = 0;
    if (!g.Get32(mgr + kMgrTypeList, &head) || !Plausible(head)) return out;
    uint32_t t = 0;
    if (!g.Get32(head, &t)) return out;
    for (uint32_t n = 0; Plausible(t) && n < kTypeCap && g.Mapped(t, kTypeName + 4); ++n) {
        TypeInfo ti;
        ti.addr = t;
        ti.name = ReadName(g, g.U32(t + kTypeName));
        ti.group = g.S32(t + kTypeGroup);
        ti.special = g.U8(t + kTypeSpecial);
        out.push_back(std::move(ti));
        uint32_t link = g.U32(t + kTypeNext);
        if (!Plausible(link) || !g.Get32(link, &t)) break;
    }
    return out;
}

std::string TypeLine(const std::vector<TypeInfo>& types, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i < to && i < types.size(); ++i) {
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s(g%d%s)", i == from ? "" : " ", types[i].name.empty() ? "?" : types[i].name.c_str(), types[i].group,
                      types[i].special ? ",special" : "");
        s += b;
    }
    return s;
}

const TypeInfo* FindType(const std::vector<TypeInfo>& types, const std::string& name) {
    for (const TypeInfo& t : types) {
        if (!t.name.empty() && EqualNoCase(t.name, name)) return &t;
    }
    return nullptr;
}

// ---------------------------------------------------------------- the player and the ambient cars

struct Pose {
    bool ok = false;
    Vec3 pos;
    double back_x = 0.0, back_z = 0.0;
    double heading = 0.0;  // the nose, scene convention
};

// The player's car: [0x8288DCF8] the vehicle, +20 its entity, whose matrix (the update copy at +144: rows right, up, back, position) is the one the
// traffic shadow hook reads. [V hooks/traffic_shadow.cpp]
Pose ReadPlayer(const Guest& g) {
    Pose p;
    uint32_t veh = 0, ent = 0;
    if (!g.Get32(kPlayerVehiclePtr, &veh) || !Plausible(veh) || !g.Get32(veh + 20, &ent) || !Plausible(ent) || !g.Mapped(ent + 144, 64)) return p;
    const uint32_t m = ent + 144;
    p.pos = Vec3{g.F32(m + 48), g.F32(m + 52), g.F32(m + 56)};
    p.back_x = g.F32(m + 32);
    p.back_z = g.F32(m + 40);
    if (!Finite3(p.pos) || !PlausibleF(float(p.pos.x)) || !PlausibleF(float(p.pos.z)) || !std::isfinite(p.back_x) || !std::isfinite(p.back_z)) return p;
    p.heading = NoseHeadingDeg(p.back_x, p.back_z);
    p.ok = true;
    return p;
}

struct Car {
    uint32_t addr = 0;
    uint32_t cls = 0;
    std::string name;
    Vec3 pos;
    double heading = 0.0;
    double dist = 0.0;  // from the player (m), -1 when no player
    double speed = -1.0;
    int32_t zone = 0;
    uint8_t flags = 0;
    uint32_t placed = 0;
    uint32_t type = 0;
    uint32_t rail = 0, rail_flags = 0, road = 0, rail_density = 0;
    float limit = 0.0f;
    bool have_rail = false;
    LodSeen lod;
};

// One ambient object of a live list. False when it cannot be read.
bool ReadCar(const Guest& g, uint32_t addr, const Pose& player, Census& census, double now, uint64_t frame, Car* out) {
    if (!Plausible(addr) || !g.Mapped(addr, kObjBytes)) return false;
    Car c;
    c.addr = addr;
    c.pos = Vec3{g.F32(addr + kObjMatrix + 48), g.F32(addr + kObjMatrix + 52), g.F32(addr + kObjMatrix + 56)};
    if (!Finite3(c.pos) || !PlausibleF(float(c.pos.x)) || !PlausibleF(float(c.pos.z))) return false;
    c.heading = NoseHeadingDeg(g.F32(addr + kObjMatrix + 32), g.F32(addr + kObjMatrix + 40));
    c.flags = g.U8(addr + kObjFlags);
    c.zone = g.S32(addr + kObjZone);
    c.placed = g.U32(addr + kObjPlaced);
    c.type = g.U32(addr + kObjType);
    c.dist = player.ok ? Distance(c.pos, player.pos) : -1.0;
    c.speed = census.Update(addr, c.pos, now);

    // The model: the type's name, through +448 (the spawn stores its type there [I]); a name that is not a name prints as '?'.
    uint32_t tname = 0;
    if (Plausible(c.type) && g.Mapped(c.type, kTypeName + 4)) tname = g.U32(c.type + kTypeName);
    c.name = ReadName(g, tname);
    // A car a layout item made carries the item's name buffer at +40 (32 bytes: the model the item was added with); it is the second place to look.
    if (c.name.empty() && c.placed != 0) c.name = PrintableName(g.base + addr + kObjPlaced, 32);
    // The rail: [[car+452]+188].
    const uint32_t nav = g.U32(addr + kObjNav);
    uint32_t rail = 0;
    if (Plausible(nav) && g.Get32(nav + kNavRail, &rail) && Plausible(rail) && g.Mapped(rail, kRailDensity + 4)) {
        c.have_rail = true;
        c.rail = rail;
        c.rail_flags = g.U32(rail + kRailFlags);
        c.rail_density = g.U32(rail + kRailDensity);
        c.road = g.U32(rail + kRailParent);
        if (Plausible(c.road) && g.Mapped(c.road, kRoadLimit + 4)) c.limit = g.F32(c.road + kRoadLimit);
    }
    c.lod = FindNote(addr, frame);
    *out = std::move(c);
    return true;
}

std::string CarLine(const Car& c, const Pose& player, const char* tag) {
    char b[640];
    char speed[24];
    if (c.speed >= 0.0) std::snprintf(speed, sizeof(speed), "%.1f", c.speed);
    else std::snprintf(speed, sizeof(speed), "?");
    char lod[96];
    if (c.lod.seen) {
        std::snprintf(lod, sizeof(lod), "%d (depth %.0f, %s, seen %llu updates ago)", c.lod.lod, double(c.lod.depth), c.lod.enabled ? "enabled" : "item off",
                      static_cast<unsigned long long>(c.lod.age));
    } else {
        std::snprintf(lod, sizeof(lod), "not seen by the draw");
    }
    char rail[200];
    if (c.have_rail) {
        std::snprintf(rail, sizeof(rail), "rail 0x%08X (flags 0x%X, road 0x%08X, limit %.1f, density 0x%08X)", c.rail, c.rail_flags, c.road, double(c.limit),
                      c.rail_density);
    } else {
        std::snprintf(rail, sizeof(rail), "rail none");
    }
    const double rel = player.ok ? HeadingDelta(player.heading, c.heading) : 0.0;
    std::snprintf(b, sizeof(b),
                  "%s 0x%08X '%s' class %u %s m at (%.1f, %.1f, %.1f) heading %.0f (to the player's %+.0f) v %s m/s zone %d flags 0x%02X placed 0x%08X type "
                  "0x%08X %s lod %s",
                  tag, c.addr, c.name.empty() ? "?" : c.name.c_str(), c.cls, c.dist >= 0.0 ? (std::to_string(int(c.dist + 0.5))).c_str() : "?", c.pos.x,
                  c.pos.y, c.pos.z, c.heading, rel, speed, c.zone, unsigned(c.flags), c.placed, c.type, rail, lod);
    return b;
}

// ---------------------------------------------------------------- state of the probe

struct State {
    bool was_on = false;
    uint64_t updates = 0;       // ticks seen
    uint64_t sample_no = 0;
    uint32_t world_updates = 0;  // ticks in a row with the manager and the player
    bool types_logged = false;
    Census census;
    Pose last_player;
    double last_player_t = 0.0;
    double player_speed = -1.0;
    // the spawn test
    int32_t spawn_done_for = 0;
    uint32_t spawn_calls = 0;
    bool follow = false;
    int32_t follow_id = -1;
    double follow_t0 = 0.0;
    Vec3 follow_point;
    uint64_t follow_next = 0;
    std::string follow_model;
    Census follow_census;
    uint32_t follow_car = 0;        // the car that came of the call (its address), 0 = none seen yet
    bool follow_car_gone = false;   // ... and it left the live list
    Vec3 follow_first_pos;
};
State g_s;

// ---------------------------------------------------------------- the sample

struct Totals {
    uint32_t cars = 0, parked = 0, peds = 0;
    uint32_t moving = 0, still = 0, unknown = 0;
    uint32_t w100 = 0, w250 = 0;
    double nearest = -1.0, farthest = -1.0;
};

// The live ambients of one class (0 = moving traffic, 1 = parked, 2 = pedestrians [I]): the cars of the class's live list.
std::vector<Car> ReadLiveCars(const Guest& g, const ManagerView& m, uint32_t cls, const Pose& player, Census& census, double now, uint64_t frame,
                              Totals* tot) {
    std::vector<Car> cars;
    if (!m.pool || cls >= kClasses) return cars;
    const uint32_t d = m.pool + cls * kDescBytes;
    if (!g.Mapped(d, kDescBytes)) return cars;
    const uint32_t head = g.U32(d + kDescLiveHead);
    WalkList(g, head, kCarCap, [&](uint32_t item, uint32_t) {
        Car c;
        if (!ReadCar(g, item, player, census, now, frame, &c)) return;
        c.cls = cls;
        cars.push_back(std::move(c));
    });
    for (const Car& c : cars) {
        ++tot->cars;
        if (c.speed < 0.0) ++tot->unknown;
        else if (c.speed > 1.0) ++tot->moving;
        else ++tot->still;
        if (c.dist >= 0.0) {
            if (c.dist <= 100.0) ++tot->w100;
            if (c.dist <= 250.0) ++tot->w250;
            if (tot->nearest < 0.0 || c.dist < tot->nearest) tot->nearest = c.dist;
            if (c.dist > tot->farthest) tot->farthest = c.dist;
        }
    }
    return cars;
}

void LogTypes(const Guest& g, const ManagerView& m) {
    const std::vector<TypeInfo> car_types = ReadCarTypes(g, m.mgr);
    const std::vector<TypeInfo> all = ReadTypeList(g, m.mgr);
    LARECOMP_APP_INFO("[traffic-probe] types: {} car types in the random-pick array (mgr+8556, group g, special = left out of the random pick), {} types in the full list (mgr+4)",
                      car_types.size(), all.size());
    for (size_t i = 0; i < car_types.size(); i += 10) LARECOMP_APP_INFO("[traffic-probe] types: car[{}..]: {}", i, TypeLine(car_types, i, i + 10));
    if (all.size() != car_types.size()) {
        for (size_t i = 0; i < all.size(); i += 10) LARECOMP_APP_INFO("[traffic-probe] types: list[{}..]: {}", i, TypeLine(all, i, i + 10));
    }
}

void LogOpponents(const Guest& g, uint32_t* opp_active, uint32_t* opp_free, uint32_t* racers) {
    *opp_active = *opp_free = *racers = 0;
    uint32_t M = 0;
    if (!g.Get32(kOpponentMgrPtr, &M) || !Plausible(M) || !g.Mapped(M, 40)) {
        LARECOMP_APP_INFO("[traffic-probe] opponents: no opponent manager");
        return;
    }
    const uint32_t active = g.U16(M + 12), racer_vec = g.U16(M + 28);
    uint32_t P = g.U32(M + 32);
    uint32_t pool_free = 0, listed = 0, p4 = 0, p8 = 0, p10 = 0;
    bool pool_ok = Plausible(P) && g.Mapped(P, 24);
    if (pool_ok) {
        pool_free = g.U16(P + 6);
        p4 = g.U16(P + 4);
        p8 = g.U16(P + 8);
        p10 = g.U16(P + 10);
        listed = WalkList(g, g.U32(P + 12), 1024, [](uint32_t, uint32_t) {});
    }
    uint32_t R = 0, rcount = 0;
    const bool rm_ok = g.Get32(kRacerMgrPtr, &R) && Plausible(R) && g.Mapped(R, 404);
    if (rm_ok) rcount = g.U32(R + 400);
    LARECOMP_APP_INFO(
        "[traffic-probe] opponents: manager 0x{:08X} active {} racers {}; free pool 0x{:08X} free count {} (free list walked {} nodes; pool u16 +4 {}, +8 {}, +10 {}; "
        "CHECK which is the capacity); racer manager 0x{:08X} racers {} (array of up to 100 [I])",
        M, active, racer_vec, pool_ok ? P : 0u, pool_free, listed, p4, p8, p10, rm_ok ? R : 0u, rcount);
    *opp_active = active;
    *opp_free = pool_free;
    *racers = rcount;
}

uint32_t LogPolice(const Guest& g) {
    uint32_t pm = 0;
    if (!g.Get32(kPoliceMgrPtr, &pm) || !Plausible(pm) || !g.Mapped(pm, 24)) {
        LARECOMP_APP_INFO("[traffic-probe] police: no police manager");
        return 0;
    }
    const uint32_t arr = g.U32(pm + 4);
    const uint32_t count = g.U16(pm + 8), render_count = g.U16(pm + 16);
    uint32_t counted = 0, alive = 0, active = 0, allow = 0, uncounted = 0;
    std::string each;
    if (Plausible(arr) && count > 0 && count <= 256 && g.Mapped(arr, 4 * count)) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t p = g.U32(arr + 4 * i);
            if (!Plausible(p) || !g.Mapped(p, kPoliceBytes)) continue;
            const bool a = g.U8(p + kPoliceAlive) != 0;
            const bool act = g.U8(p + kPoliceActive) != 0;
            const bool al = g.U8(p + kPoliceAllow) != 0;
            const bool flagged = g.U32(p + kPoliceFlag) != 0;
            if (flagged) ++uncounted;
            else ++counted;
            if (a) ++alive;
            if (act) ++active;
            if (al) ++allow;
            if (i < kPoliceShown) {
                char b[80];
                std::snprintf(b, sizeof(b), "%s0x%08X:alive%d/act%d/allow%d%s", each.empty() ? "" : " ", p, a, act, al, flagged ? "/flag" : "");
                each += b;
            }
        }
    }
    LARECOMP_APP_INFO(
        "[traffic-probe] police: manager 0x{:08X} update array 0x{:08X} count {} (render copy count {}); counted {} (the game's own count: entries with +1152 "
        "clear), alive(+872) {}, active(+1072) {}, allowed to patrol(+1073) {}, flagged {}; {}",
        pm, arr, count, render_count, counted, alive, active, allow, uncounted, each.empty() ? "no police" : each);
    return counted;
}

// One full sample (the cvar's N updates apart).
void Sample(const Guest& g, const ManagerView& m, const Pose& player, double now) {
    State& s = g_s;
    ++s.sample_no;
    const uint64_t frame = s.updates;

    // The player and the density chain.
    const double scale = REXCVAR_GET(traffic_density_scale);
    char zone_text[260] = "zone 0 unreadable";
    if (m.mgr) {
        const uint32_t z = m.mgr + kMgrZones;
        if (g.Mapped(z, kZoneEnabled + 1)) {
            std::snprintf(zone_text, sizeof(zone_text),
                          "zone 0: density factor %.3f (+960), player speed %.1f (+968), slot %d, sums +1796 %.3f (0x%08X) / +1800 %.3f (0x%08X), enabled byte %u",
                          double(g.F32(z + kZoneFactor)), double(g.F32(z + kZoneSpeed)), g.S32(z + 972), double(g.F32(z + kZoneSumA)), g.U32(z + kZoneSumA),
                          double(g.F32(z + kZoneSumB)), g.U32(z + kZoneSumB), unsigned(g.U8(z + kZoneEnabled)));
        }
    }
    const uint8_t b_traffic = g.Mapped(kTrafficEnableByte, 3) ? g.U8(kTrafficEnableByte) : 255;
    const uint8_t b_ped = g.Mapped(kPedEnableByte, 1) ? g.U8(kPedEnableByte) : 255;
    const uint8_t b_parked = g.Mapped(kParkedEnableByte, 1) ? g.U8(kParkedEnableByte) : 255;
    const uint8_t b_layout = (m.mgr && g.Mapped(m.mgr + kMgrParkedLayoutByte, 1)) ? g.U8(m.mgr + kMgrParkedLayoutByte) : 255;
    if (player.ok) {
        LARECOMP_APP_INFO(
            "[traffic-probe] sample {} (update {}): player at ({:.1f}, {:.1f}, {:.1f}) heading {:.0f} speed {:.1f} m/s; traffic_density_scale {:.2f}; enable bytes "
            "traffic {} peds {} parked {} (the parked spawner's layout byte {}); {}",
            s.sample_no, frame, player.pos.x, player.pos.y, player.pos.z, player.heading, s.player_speed, scale, unsigned(b_traffic), unsigned(b_ped),
            unsigned(b_parked), unsigned(b_layout), zone_text);
    } else {
        LARECOMP_APP_INFO("[traffic-probe] sample {} (update {}): NO player car readable; traffic_density_scale {:.2f}; enable bytes traffic {} peds {} parked {} (layout byte {}); {}",
                          s.sample_no, frame, scale, unsigned(b_traffic), unsigned(b_ped), unsigned(b_parked), unsigned(b_layout), zone_text);
    }

    // The pools.
    PoolCounts amb[kClasses], lay[kClasses];
    for (uint32_t c = 0; c < kClasses; ++c) {
        amb[c] = ReadDesc(g, m.pool, c);
        lay[c] = ReadDesc(g, m.layout_pool, c);
    }
    LARECOMP_APP_INFO("[traffic-probe] pool ambient (live ambients) 0x{:08X}: class0 {} | class1 {} | class2 {}", m.pool, DescText(amb[0]), DescText(amb[1]),
                      DescText(amb[2]));
    LARECOMP_APP_INFO("[traffic-probe] pool layout (AddAmbientToLayout's) 0x{:08X} (layout data 0x{:08X}, same descriptor shape [I]): class0 {} | class1 {} | class2 {}",
                      m.layout_pool, m.layout_data, DescText(lay[0]), DescText(lay[1]), DescText(lay[2]));

    uint32_t opp_active = 0, opp_free = 0, racers = 0;
    LogOpponents(g, &opp_active, &opp_free, &racers);
    const uint32_t police = LogPolice(g);

    // The cars.
    s.census.Begin();
    Totals tot;
    std::vector<Car> cars = ReadLiveCars(g, m, 0, player, s.census, now, frame, &tot);
    s.census.End();
    tot.parked = amb[1].read ? amb[1].live : 0;
    tot.peds = amb[2].read ? amb[2].live : 0;
    uint32_t drawn_seen = 0, drawn_inview = 0;
    CountNotes(frame, 120, &drawn_seen, &drawn_inview);
    LARECOMP_APP_INFO(
        "[traffic-probe] census: {} cars read from the class 0 live list (its live count {}), parked(class 1 live) {}, peds(class 2 live) {}; new since the last sample "
        "{}, gone {}; moving (>1 m/s) {}, still {}, speed unknown {}; within 100 m {}, within 250 m {}; nearest {:.0f} m, farthest {:.0f} m; the traffic draw saw {} different cars in the last 120 updates ({} of them in view with the item on)",
        tot.cars, amb[0].live, tot.parked, tot.peds, s.census.New(), s.census.Gone(), tot.moving, tot.still, tot.unknown, tot.w100, tot.w250, tot.nearest,
        tot.farthest, drawn_seen, drawn_inview);

    const int detail = REXCVAR_GET(mcla_traffic_probe_detail);
    if (detail > 0 && !cars.empty()) {
        std::sort(cars.begin(), cars.end(), [](const Car& a, const Car& b) {
            const double da = a.dist < 0.0 ? 1.0e12 : a.dist, db = b.dist < 0.0 ? 1.0e12 : b.dist;
            return da < db;
        });
        const size_t shown = std::min<size_t>(cars.size(), detail >= 2 ? kAllShown : kNearestShown);
        for (size_t i = 0; i < shown; ++i) LARECOMP_APP_INFO("{}", CarLine(cars[i], player, "[traffic-probe] car"));
        if (cars.size() > shown) LARECOMP_APP_INFO("[traffic-probe] car: {} more cars not listed (mcla_traffic_probe_detail 2 lists them)", cars.size() - shown);
    }

    LARECOMP_APP_INFO(
        "[traffic-probe] SUMMARY sample={} update={} scale={:.2f} traffic_byte={} cars={} parked={} peds={} new={} gone={} moving={} still={} w100={} w250={} "
        "amb_free={} amb_live={} amb_pend={} amb_used={} layout_free={} layout_live={} layout_pend={} layout_used={} opp_active={} opp_free={} racers={} police={} "
        "drawn={} inview={}",
        s.sample_no, frame, scale, unsigned(b_traffic), tot.cars, tot.parked, tot.peds, s.census.New(), s.census.Gone(), tot.moving, tot.still, tot.w100, tot.w250,
        amb[0].free, amb[0].live, amb[0].pending, amb[0].used, lay[0].free, lay[0].live, lay[0].pending, lay[0].used, opp_active, opp_free, racers, police,
        drawn_seen, drawn_inview);
}

// ---------------------------------------------------------------- the spawn test

// Where an item with this id is: its list's name and the item, in the layout pool's class 0.
struct ItemHit {
    bool found = false;
    uint32_t item = 0;
    const char* list = "";
};

ItemHit FindLayoutItem(const Guest& g, const ManagerView& m, int32_t id) {
    ItemHit hit;
    if (!m.layout_pool || !g.Mapped(m.layout_pool, kDescBytes)) return hit;
    struct L {
        uint32_t off;
        const char* name;
    };
    const L lists[] = {{kDescUsedHead, "used"}, {kDescLiveHead, "live"}, {kDescPendingHead, "pending"}, {kDescFreeHead, "free"}};
    for (const L& l : lists) {
        WalkList(g, g.U32(m.layout_pool + l.off), kListCap, [&](uint32_t item, uint32_t) {
            if (hit.found || !Plausible(item) || !g.Mapped(item, kItemBytes)) return;
            if (g.S32(item + kItemId) == id) {
                hit.found = true;
                hit.item = item;
                hit.list = l.name;
            }
        });
        if (hit.found) break;
    }
    return hit;
}

void LogFollow(const Guest& g, const ManagerView& m, const Pose& player, double now, uint64_t frame) {
    State& s = g_s;
    const double t = now - s.follow_t0;
    const ItemHit hit = FindLayoutItem(g, m, s.follow_id);
    if (hit.found) {
        const uint32_t it = hit.item;
        const uint32_t vt = g.U32(it);
        uint32_t slot4 = 0;
        if (Plausible(vt) && g.Mapped(vt, 24)) slot4 = g.U32(vt + 16);
        LARECOMP_APP_INFO(
            "[traffic-probe] spawn +{:.1f} s: layout item 0x{:08X} id {} is in the '{}' list of class 0; name '{}', at ({:.1f}, {:.1f}, {:.1f}), heading {:.3f} rad, "
            "+76 bound 0x{:08X}, +80 state {}, +140 byte {}, vtable 0x{:08X} slot 4 (the call AddAmbientToLayout makes on it) 0x{:08X}",
            t, it, s.follow_id, hit.list, ReadName(g, it + kItemName, 32), double(g.F32(it + kItemPos)), double(g.F32(it + kItemPos + 4)),
            double(g.F32(it + kItemPos + 8)), double(g.F32(it + kItemHeading)), g.U32(it + kItemBound), g.S32(it + kItemState), unsigned(g.U8(it + kItemByte)), vt,
            slot4);
    } else {
        LARECOMP_APP_INFO("[traffic-probe] spawn +{:.1f} s: the layout item with id {} is in none of the class 0 lists (it may have been given back)", t,
                          s.follow_id);
    }
    // The cars: the one that came of the call is tracked by its address (it is picked from the cars within 8 m of the spawn point, a placed one
    // (+40 non-zero) first), so a car that drives off is still followed and one that is not in the live list any more is reported as gone.
    s.follow_census.Begin();
    Totals tot;
    std::vector<Car> cars;
    for (uint32_t cls = 0; cls < kClasses; ++cls) {  // a layout item's car may well be a parked one (class 1): look in every class
        std::vector<Car> part = ReadLiveCars(g, m, cls, player, s.follow_census, now, frame, &tot);
        cars.insert(cars.end(), part.begin(), part.end());
    }
    s.follow_census.End();
    char tag[220];
    if (s.follow_car != 0) {
        const Car* mine = nullptr;
        for (const Car& c : cars) {
            if (c.addr == s.follow_car) mine = &c;
        }
        if (mine) {
            std::snprintf(tag, sizeof(tag), "[traffic-probe] spawn +%.1f s: the placed car (%.1f m from the spawn point, %.1f m from where it was first seen):", t,
                          Distance(mine->pos, s.follow_point), Distance(mine->pos, s.follow_first_pos));
            LARECOMP_APP_INFO("{}", CarLine(*mine, player, tag));
        } else {
            LARECOMP_APP_INFO("[traffic-probe] spawn +{:.1f} s: the placed car 0x{:08X} is not in any live ambient list any more (despawned, or moved to another list)", t,
                              s.follow_car);
            s.follow_car = 0;
            s.follow_car_gone = true;
        }
    }
    if (s.follow_car == 0 && !s.follow_car_gone) {
        const Car* pick = nullptr;
        double best = 1.0e12;
        for (const Car& c : cars) {
            const double d = Distance(c.pos, s.follow_point);
            if (d > kNearSpawnM) continue;
            const double key = d - (c.placed != 0 ? 1000.0 : 0.0);  // a placed car beats any other
            if (key < best) {
                best = key;
                pick = &c;
            }
        }
        if (pick) {
            s.follow_car = pick->addr;
            s.follow_first_pos = pick->pos;
            std::snprintf(tag, sizeof(tag), "[traffic-probe] spawn +%.1f s: AN AMBIENT APPEARED within %.0f m of the spawn point (%s; %.1f m away):", t, kNearSpawnM,
                          pick->placed != 0 ? "placed: +40 non-zero" : "NOT marked placed: it may be a traffic car passing", Distance(pick->pos, s.follow_point));
            LARECOMP_APP_INFO("{}", CarLine(*pick, player, tag));
        } else {
            LARECOMP_APP_INFO("[traffic-probe] spawn +{:.1f} s: no ambient (class 0-2) within {:.0f} m of the spawn point ({} live in the three lists)", t, kNearSpawnM, tot.cars);
        }
    }
}

// The whole spawn test for one set. `player` is valid here.
void TrySpawn(const Guest& g, const ManagerView& m, const Pose& player, double now, int32_t value) {
    State& s = g_s;
    const std::vector<TypeInfo> all = ReadTypeList(g, m.mgr);
    const std::vector<TypeInfo> car_types = ReadCarTypes(g, m.mgr);

    // The model: the cvar's, else the first regular car type.
    std::string wanted = REXCVAR_GET(mcla_traffic_probe_model);
    std::string source = "mcla_traffic_probe_model";
    if (wanted.empty()) {
        source = "the first regular car type of the game's list";
        for (const TypeInfo& t : car_types) {
            if (!t.name.empty() && t.special == 0 && FitsLayoutName(t.name)) {
                wanted = t.name;
                break;
            }
        }
    }
    const TypeInfo* known = wanted.empty() ? nullptr : FindType(all, wanted);
    if (known && FitsLayoutName(known->name)) wanted = known->name;  // the game's own spelling

    const double ahead = REXCVAR_GET(mcla_traffic_probe_ahead);
    Vec3 point;
    const bool have_dir = PointAhead(player.pos, player.back_x, player.back_z, ahead, &point);
    const double native_heading = NativeHeadingDeg(player.back_x, player.back_z);

    const PoolCounts layout0 = ReadDesc(g, m.layout_pool, 0);
    SpawnInputs in;
    in.manager = m.Ok();
    in.player = player.ok;
    in.free_roam = RpcInFreeRoam();
    in.driving = larecomp::triggers::IsDriving();
    in.world_updates = s.world_updates;
    in.layout_pool = layout0.Sane();
    in.layout_free = layout0.free;
    in.type_known = known != nullptr && FitsLayoutName(wanted);
    in.position_ok = have_dir && PlausibleSpawnPoint(point, player.pos);
    in.calls_so_far = s.spawn_calls;
    const Refusal why = DecideSpawn(in);

    LARECOMP_APP_INFO(
        "[traffic-probe] spawn: set {}: model '{}' ({}), {:.0f} m ahead of the player at ({:.1f}, {:.1f}, {:.1f}), heading {:.0f} (the player's native heading; the "
        "layout's own convention is CHECK); layout class 0 {}; free roam {}, driving {}, world updates {}; the parked spawner that turns a layout item into a car: enable byte {} "
        "(parked_car_scale {:.2f}), layout byte {}",
        value, wanted, source, ahead, point.x, point.y, point.z, native_heading, DescText(layout0), in.free_roam, in.driving, in.world_updates,
        unsigned(g.Mapped(kParkedEnableByte, 1) ? g.U8(kParkedEnableByte) : 255), REXCVAR_GET(parked_car_scale),
        unsigned((m.mgr && g.Mapped(m.mgr + kMgrParkedLayoutByte, 1)) ? g.U8(m.mgr + kMgrParkedLayoutByte) : 255));
    if (why != Refusal::kNone) {
        LARECOMP_APP_INFO("[traffic-probe] spawn: REFUSED: {} (the set is used up; set the cvar to 0 and to another number to try again)", RefusalText(why));
        return;
    }

    ++s.spawn_calls;
    const uint32_t name_ptr = mc::cinematic::GuestPutString(wanted.c_str());
    const uint32_t vec_ptr = mc::cinematic::GuestPutVec(float(point.x), float(point.y), float(point.z), 0.0f);
    if (name_ptr == 0 || vec_ptr == 0) {
        LARECOMP_APP_INFO("[traffic-probe] spawn: no guest memory for the arguments (the native call shim has no block yet); nothing was called");
        return;
    }
    const mc::cinematic::NativeResult r = mc::cinematic::CallNative(kAddAmbientToLayout, {name_ptr, vec_ptr, std::bit_cast<uint32_t>(float(native_heading))});
    const PoolCounts after = ReadDesc(g, m.layout_pool, 0);
    const int32_t id = int32_t(r.ret);
    LARECOMP_APP_INFO(
        "[traffic-probe] spawn: AmbientMgr_AddAmbientToLayout called={} returned {} (0x{:08X}; -1 = the game's refusal: no manager, no name, or its pool is empty) "
        "ret_written={} in {:.2f} ms; layout class 0 now {}",
        r.called, id, r.ret, r.ret_written, r.ms, DescText(after));
    if (!r.called || !r.ret_written || id < 0) return;
    s.follow = true;
    s.follow_id = id;
    s.follow_t0 = now;
    s.follow_point = point;
    s.follow_model = wanted;
    s.follow_census.Clear();
    s.follow_car = 0;
    s.follow_car_gone = false;
    s.follow_next = s.updates;  // a line at once
}

// ---------------------------------------------------------------- the tick

void ResetWorld() {
    State& s = g_s;
    s.world_updates = 0;
    s.census.Clear();
    s.last_player = Pose{};
    s.player_speed = -1.0;
}

}  // namespace

// Once an update (Patch_DeltaTimePre, game thread): the spawn test's cvar, the probe's samples, the follow-up of a spawn.
void TickTrafficProbe() {
    State& s = g_s;
    ++s.updates;
    g_note_frame.store(s.updates, std::memory_order_relaxed);

    const int32_t interval = REXCVAR_GET(mcla_traffic_probe);
    const int32_t spawn = REXCVAR_GET(mcla_traffic_probe_spawn);
    const bool probe_on = interval > 0;
    const bool following = s.follow;
    const bool want_spawn = spawn != 0 && spawn != s.spawn_done_for;
    if (spawn == 0) s.spawn_done_for = 0;

    // Off: say so once, and keep the draw hook quiet.
    if (!probe_on && !following && !want_spawn) {
        if (s.was_on) {
            LARECOMP_APP_INFO("[traffic-probe] off");
            s.was_on = false;
            g_note_on.store(false, std::memory_order_relaxed);
            s.types_logged = false;
            ResetWorld();
        }
        return;
    }

    auto* rt = rex::Runtime::instance();
    Guest g;
    g.base = rt ? rt->virtual_membase() : nullptr;
    if (!g.base) return;
    g_note_base.store(g.base, std::memory_order_relaxed);
    g_note_on.store(true, std::memory_order_relaxed);
    if (!s.was_on) {
        s.was_on = true;
        LARECOMP_APP_INFO("[traffic-probe] on: every {} updates; detail {}; read only (the spawn cvar is {})", interval, REXCVAR_GET(mcla_traffic_probe_detail),
                          spawn == 0 ? "idle" : "set");
    }

    const double now = NowSeconds();
    const ManagerView m = ReadManager(g);
    const Pose player = m.Ok() ? ReadPlayer(g) : Pose{};
    if (!m.Ok() || !player.ok) {
        if (s.world_updates != 0) LARECOMP_APP_INFO("[traffic-probe] the world is gone (manager {}, player car {}); the census starts again", m.Ok(), player.ok);
        ResetWorld();
    } else {
        ++s.world_updates;
        if (s.last_player.ok && now > s.last_player_t + 0.02) {
            s.player_speed = DistanceXZ(s.last_player.pos, player.pos) / (now - s.last_player_t);
        }
        if (!s.last_player.ok || now > s.last_player_t + 0.02) {
            s.last_player = player;
            s.last_player_t = now;
        }
    }

    // The spawn test: one set, one call (or one refusal).
    if (want_spawn) {
        NewSpawnSet(spawn, s.spawn_done_for);
        if (m.Ok() && player.ok) {
            TrySpawn(g, m, player, now, spawn);
        } else {
            LARECOMP_APP_INFO("[traffic-probe] spawn: set {}: REFUSED: no ambient manager or no player car (the set is used up)", spawn);
        }
    }

    if (!m.Ok() || !player.ok) return;

    // The probe's own samples.
    if (probe_on && (s.updates % uint64_t(interval)) == 0) {
        if (!s.types_logged) {
            s.types_logged = true;
            LogTypes(g, m);
        }
        Sample(g, m, player, now);
    }

    // The follow-up of a spawn: every probe interval, or every 30 updates when the probe is off.
    if (s.follow) {
        const double t = now - s.follow_t0;
        if (t > kFollowSeconds) {
            LARECOMP_APP_INFO("[traffic-probe] spawn: the follow-up of id {} ends after {:.0f} s", s.follow_id, kFollowSeconds);
            s.follow = false;
        } else if (s.updates >= s.follow_next) {
            LogFollow(g, m, player, now, s.updates);
            s.follow_next = s.updates + (probe_on ? uint64_t(interval) : uint64_t(kFollowEvery));
        }
    }
}

// The draw hook's note (traffic_shadow.cpp calls it at the entry of the traffic item draw, r3 = the item): the LOD the item holds, by its
// car, so the sample can print it. Reads four words of the item the draw is about to read anyway.
void TrafficProbeNoteItem(uint32_t item) {
    if (!g_note_on.load(std::memory_order_relaxed)) return;
    const uint8_t* base = g_note_base.load(std::memory_order_relaxed);
    if (!base || !Plausible(item)) return;
    const uint32_t car = ReadGuestU32(base, item + 16);
    if (!Plausible(car)) return;
    const uint64_t frame = g_note_frame.load(std::memory_order_relaxed);
    const size_t h = NoteHash(car);
    NoteSlot* take = &g_notes[h];
    for (uint32_t i = 0; i < kNoteProbes; ++i) {
        NoteSlot& s = g_notes[(h + i) & (kNoteSlots - 1)];
        const uint32_t c = s.car.load(std::memory_order_relaxed);
        if (c == car || c == 0) {
            take = &s;
            break;
        }
        if (s.frame.load(std::memory_order_relaxed) + 600 < frame) take = &s;  // a stale one may be replaced
    }
    take->car.store(car, std::memory_order_relaxed);
    take->lod.store(int32_t(ReadGuestU32(base, item + 28)), std::memory_order_relaxed);
    take->depth_bits.store(ReadGuestU32(base, item + 32), std::memory_order_relaxed);
    take->enabled.store(base[item + 36], std::memory_order_relaxed);
    take->frame.store(frame, std::memory_order_relaxed);
}

#else  // REXGLUE_HAS_XEO3_TARGET
#include <cstdint>
void TickTrafficProbe() {}
void TrafficProbeNoteItem(uint32_t) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
