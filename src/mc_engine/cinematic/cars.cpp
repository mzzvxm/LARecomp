#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The scene's cars in the game (trailer toolkit, work packages M4b and M4d): see cars.h. The statements are in car_verbs.h, the order
// of the native calls in car_spawn.h (a pure script, tested without the game); what is here is making the calls and reading the poses.
//
// WHERE AND WHEN. The scene runner (scene_runtime.cpp) hands the `car` keys that come due to the applier registered here; the applier
// only queues them in the Director. TickCars, from Patch_DeltaTimePre on the game thread (one line in hooks/hooks.cpp), makes the
// calls, at most ONE native an update, and only while: the player's car is on the road, the ambient manager and the opponent
// manager exist, the game is not paused (not in a menu, the garage or a loading screen), and that has held for 10 updates. The natives
// are the ones E4 called from the same point in free roam (test/cin-e4, CIN-E4-ROUND3.md: 158 calls, no fault). A racer that is
// called on has been looked up in the opponent manager's active list first: when the world is rebuilt under a scene (a race, the
// front end) the car is simply gone from the books and nothing is called on it.
//
// WHAT IS WRITTEN. Nothing but through the natives: OpponentManager_CreateOpponent / Load / Add / Delete, Racer_Activate /
// SetStreamingLockedIn / Teleport (the spawn), Racer_ChangeCarByConfig on the player's racer (the garage dressing); the reads are
// LookupCar, LocalOptions_*, Racer_GetLoadState / IsCarStreamed / GetPosition and the car table of the game's memory (the name
// string of a car). No guest memory is poked. The scene's end deletes what the scene spawned and puts the player's car back.
//
// AMBIENT CARS. `car <id> ambient <model> at <x,y,z> [heading <deg>]` makes a real traffic-class car through AmbientMgr_AddAmbientToLayout (the layout item the game's
// parked spawner turns into a car within a second; ambient_car.h has what the traffic probe proved and what the removal does). The same Director hands out the add and,
// at the end of the scene or on `car <id> delete`, AmbientMgr_RemoveAmbientFromLayout(the item's id), one native an update, and only while the guards of ambient_car.h hold
// (free roam, a layout slot to spare, the world up for 120 updates; the manager and the player's car exist, the game is not paused). The model is looked up in the
// ambient manager's type list first (its own spelling goes to the native) and the point must be within a kilometre of the player. The car's matrix is read through its layout
// item (the walk of the layout pool's lists by id, then item+76 the bound car, car+128 the update copy of its matrix) so that the registry, the camera's `car:<id>` and
// the state log see it; the first read is logged beside what the scene asked for.
//
// PATHS (M4c). `car <id> path <route> [speed <x>] [mode brain|kinematic]` and `car <id> stop`. The route file is read when the scene starts (beside the
// scene file, or in a `routes` folder beside it). Kinematic: every update, for each car that has a path and is ready, one Racer_Teleport(racer, place,
// heading, speed) at the place the route has at the PATH CLOCK (the scene's time with the scene's time scale applied, integrated here: a car follows a
// slowed-down world); the first one puts the car on the route's first point. Brain: BrainFactory_CreateBrainCruising, SetSpeed, SetDestSpeed,
// SetLooping(false), AppendDest x N, BrainCruising_DriveToNearestRail (car_spawn.h), one native an update through the same Director as the spawn
// steps; the brain's state byte and destination index (brain+1708, brain+1656) are READ from the guest memory every update for the log. Nothing in the
// guest memory is written by the host.
//
// THE POSE (M4d). racer+48 -> entity, entity+28 -> matrix, 16 floats from matrix+16: right, up, BACK, position. That is what E4's
// matrix route read for the player and for the spawned car (racer+0x470 holds the 0xCDCDCDCD fill outside a race). Every update the
// matrix of each car that exists is read into the registry; GetCarPose / CarMatrixById read it again at the moment they are asked.
// Racer_GetPosition is called once after a car is placed and compared with the matrix (the log line says how far apart they are).
//
// Written without a build or a run in the game. CHECK items are marked where nothing has shown the behaviour in a running game.
//

#include "cars.h"

#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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

#include "car_spawn.h"
#include "director/director_math.h"
#include "discord_rpc/discord_rpc.h"  // RpcInFreeRoam: the game's state machine is in Cruising
#include "larecomp_log.h"
#include "native_call.h"
#include "scene_runtime.h"
#include "scene_seed_runtime.h"
#include "larecomp_guest.h"

#if __has_include("camera_runtime.h")
#include "camera_runtime.h"  // M1b: SetCarResolver. Present once its branch is merged; then the registry is the camera's car source
#define LARECOMP_CARS_HAS_CAMERA_RUNTIME 1
#endif
#if __has_include("state_log.h")
#include "state_log.h"  // M3a: RegisterCarSource. Present once its branch is merged; then the registry is the state log's source for the scene's cars
#define LARECOMP_CARS_HAS_STATE_LOG 1
#endif

namespace mc::cinematic {

namespace {

// ---------------------------------------------------------------- guest addresses (the generated code and the notes of E4)

constexpr uint32_t kPauseStatePtr = 0x828CCEA0;     // +4 paused, +10 run-one-frame
constexpr uint32_t kAmbientMgrPtr = 0x8286D8E4;
constexpr uint32_t kPlayerMgrPtr = 0x82874374;
constexpr uint32_t kPlayerValidFn = 0x821E80F8;     // (playerMgr, index) -> bool
constexpr uint32_t kPlayerRacerFn = 0x822A3998;     // (playerMgr, index) -> racer
constexpr uint32_t kPlayerSweep = 4;
constexpr uint32_t kOpponentMgrPtr = 0x8286EBE4;    // the opponent manager: *(0x8286EBE4); +8 the active array, u16 count at +12
constexpr uint32_t kMgrActiveArrayAt = 8;
constexpr uint32_t kMgrActiveCountAt = 12;
constexpr uint32_t kCarTable = 0x828CD0A8;          // LookupCar's table: +0 records, +4 u16 count; 104-byte records, +0 the name pointer
constexpr uint32_t kCarRecordBytes = 104;
constexpr uint32_t kRacerEntityAt = 48;
constexpr uint32_t kEntityMatrixAt = 28;
constexpr uint32_t kBrainDestAt = 1656;   // BrainCruising: the index of the destination it heads for (-1: none); sub_82746340
constexpr uint32_t kBrainStateAt = 1708;  // ... and its state (0 idle with the brake held, 4 no lane, 6 goal reached)

// The ambient manager (0x8286D8E4): +4 the type list, +410144 (0x64220) the layout object, whose [+0] is the layout data and [data+32] the layout's pool: 52-byte
// descriptors, one a class (class 0 = the cars); a descriptor's +16 is its free count, +8 the head of the live list and +32 of the used list (nodes: item, next at +4).
// A layout item: +76 the car bound to it, +136 its id; a live ambient car: +128 the update copy of its matrix (right, up, back, position). The traffic probe's reading of the same.
constexpr uint32_t kMgrTypeListAt = 4;
constexpr uint32_t kMgrLayoutAt = 410144;
constexpr uint32_t kPoolDescBytes = 52;
constexpr uint32_t kDescLiveHead = 8;
constexpr uint32_t kDescFreeCount = 16;
constexpr uint32_t kDescUsedHead = 32;
constexpr uint32_t kTypeNameAt = 172;
constexpr uint32_t kTypeNextAt = 20;
constexpr uint32_t kItemBound = 76;
constexpr uint32_t kItemId = 136;
constexpr uint32_t kItemBytes = 144;
constexpr uint32_t kAmbientMatrixAt = 128;
constexpr uint32_t kAmbientCarBytes = 192;
constexpr uint32_t kListCap = 600;  // nodes walked in one list at the most
constexpr uint32_t kTypeCap = 512;

constexpr int kSettleTicks = 10;           // updates the world must have been ready before a native is called
constexpr int kWorldCheckEvery = 15;       // updates between looking for lost cars in the opponent manager
constexpr int64_t kPoseLogEveryMs = 2000;

struct NativeDef {
    Native native;
    uint32_t wrapper;  // feature-parity/natives/natives.tsv
};
constexpr NativeDef kNatives[] = {
    {Native::kLookupCar, 0x822C9E68},             // LookupCar
    {Native::kNumProfiles, 0x8238C3F0},           // LocalOptions_GetNumValidVehicleProfiles
    {Native::kCurrentProfile, 0x8238C370},        // LocalOptions_GetCurrentVehicleProfileIndex
    {Native::kGetProfile, 0x8238CB28},            // LocalOptions_GetVehicleProfile
    {Native::kCreateOpponent, 0x822B8820},        // OpponentManager_CreateOpponent
    {Native::kLoadOpponent, 0x822B88A8},          // OpponentManager_LoadOpponent
    {Native::kLoadOpponentByConfig, 0x822B88B8},  // OpponentManager_LoadOpponentByCarConfig
    {Native::kAddOpponent, 0x822B7F80},           // OpponentManager_AddOpponentToWorld
    {Native::kGetLoadState, 0x822AD790},          // Racer_GetLoadState
    {Native::kIsCarStreamed, 0x822AD0B8},         // Racer_IsCarStreamed
    {Native::kActivate, 0x822AD1F0},              // Racer_Activate
    {Native::kLockStreaming, 0x822AD6E0},         // Racer_SetStreamingLockedIn
    {Native::kTeleport, 0x822AF0F8},              // Racer_Teleport
    {Native::kGetPosition, 0x822AEF88},           // Racer_GetPosition
    {Native::kDeleteOpponent, 0x822B7F90},        // OpponentManager_DeleteOpponent
    {Native::kChangeCarByConfig, 0x822AD9D8},     // Racer_ChangeCarByConfig
    {Native::kCreateBrain, 0x82742A78},           // BrainFactory_CreateBrainCruising
    {Native::kBrainSetSpeed, 0x82745858},         // BrainCruising_SetSpeed
    {Native::kBrainSetDestSpeed, 0x82745870},     // BrainCruising_SetDestSpeed
    {Native::kBrainSetLooping, 0x82745888},       // BrainCruising_SetLooping
    {Native::kBrainAppendDest, 0x827460A8},       // BrainCruising_AppendDest
    {Native::kBrainDrive, 0x827469D8},            // BrainCruising_DriveToNearestRail
    {Native::kBrainReached, 0x82745830},          // BrainCruising_HasReachedGoal
    {Native::kBrainStop, 0x827457F0},             // BrainCruising_RemoveFromTraffic
    {Native::kDeleteBrain, 0x82742AD8},           // BrainFactory_DeleteBrain
    {Native::kAddAmbient, 0x826D6850},            // AmbientMgr_AddAmbientToLayout
    {Native::kRemoveAmbient, 0x826D5CD8},         // AmbientMgr_RemoveAmbientFromLayout
};

uint32_t WrapperFor(Native n) {
    for (const NativeDef& d : kNatives) {
        if (d.native == n) return d.wrapper;
    }
    return 0;
}

// ---------------------------------------------------------------- guest memory

uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

// Is [ea, ea + len) committed host memory (readable)? A racer a test file or a stale list gave may well not be one.
bool Mapped(uint32_t ea, uint32_t len) {
    uint8_t* b = Membase();
    if (!b || ea < 0x10000 || len == 0 || uint64_t(ea) + len > 0x100000000ull) return false;
#if defined(_WIN32)
    constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY;
    uint64_t at = ea;
    const uint64_t end = uint64_t(ea) + len;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(b + at, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kRead) == 0) return false;
        const uint64_t region_end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - b) + mbi.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    return true;
#else
    // No VirtualQuery: ask the kernel to read one byte of every page. process_vm_readv on this very process answers an
    // unmapped or PROT_NONE page (guest memory that is reserved and not committed) with EFAULT, where reading it would
    // fault the game.
    constexpr uint64_t kPage = 0x1000;
    const uint64_t end = uint64_t(ea) + len;
    for (uint64_t page = uint64_t(ea) & ~(kPage - 1); page < end; page += kPage) {
        const uint64_t at = std::max<uint64_t>(page, ea);
        uint8_t probe = 0;
        iovec local{&probe, 1};
        iovec remote{b + at, 1};
        const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (n < 0 && (errno == ENOSYS || errno == EPERM)) return ea < 0xC0000000u;  // the call is not allowed here: the plausible range only
        if (n != 1) return false;
    }
    return true;
#endif
}

uint32_t R32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}

uint32_t R16(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea]) << 8) | base[ea + 1];
}

float RF(const uint8_t* base, uint32_t ea) {
    return std::bit_cast<float>(R32(base, ea));
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = REX_RUNTIME();
    if (!rt) return nullptr;
    return rt->FindFunction(addr).value_or(nullptr);
}

bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;  // the test map_warp.cpp uses
}

// The player's racer (0 in a menu, the garage, a loading screen, or off a guest thread).
uint32_t PlayerRacer(const uint8_t* base) {
    if (!rex::kernel::ThreadState::Get()) return 0;
    const uint32_t mgr = R32(base, kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(larecomp::guest::Call<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = larecomp::guest::Call<uint32_t>(racer_fn, mgr, i)) return r;
    }
    return 0;
}

// Where a read of a car's matrix got to, for the log.
struct MatrixRead {
    uint32_t entity = 0;
    uint32_t matrix = 0;
    const char* stage = "";  // which pointer of the chain stopped it ("" when the 16 words were read)
};

// The car's live matrix (rows right, up, back, position), or false: the entity does not exist (yet, or any more). The words are what the game holds,
// NaN w words and all (PoseFromMatrix and the registry ignore those).
bool ReadCarMatrix(const uint8_t* base, uint32_t racer, float out[16], MatrixRead* info = nullptr) {
    MatrixRead local;
    MatrixRead& r = info ? *info : local;
    r = MatrixRead{};
    if (!Mapped(racer + kRacerEntityAt, 4)) {
        r.stage = "the racer is not readable";
        return false;
    }
    r.entity = R32(base, racer + kRacerEntityAt);
    if (!Mapped(r.entity + kEntityMatrixAt, 4)) {
        r.stage = r.entity ? "the entity is not readable (racer+48)" : "the racer has no entity (racer+48 is 0)";
        return false;
    }
    r.matrix = R32(base, r.entity + kEntityMatrixAt);
    if (!Mapped(r.matrix + 16, 64)) {
        r.stage = r.matrix ? "the matrix is not readable (entity+28)" : "the entity has no matrix (entity+28 is 0)";
        return false;
    }
    for (uint32_t i = 0; i < 16; ++i) out[i] = RF(base, r.matrix + 16 + 4 * i);
    return true;
}

// Is `racer` one of the opponent manager's active racers?
bool InManager(const uint8_t* base, uint32_t mgr, uint32_t racer) {
    if (!mgr || !racer || !Mapped(mgr, kMgrActiveCountAt + 2)) return false;
    const uint32_t count = R16(base, mgr + kMgrActiveCountAt);
    const uint32_t arr = R32(base, mgr + kMgrActiveArrayAt);
    if (count == 0 || count > 4096 || !Mapped(arr, 4 * count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (R32(base, arr + 4 * i) == racer) return true;
    }
    return false;
}

// The brain's state byte and the index of its destination, or false when the brain's memory is not readable.
bool ReadBrain(const uint8_t* base, uint32_t brain, int* state, int* dest) {
    if (!brain || !Mapped(brain + kBrainDestAt, kBrainStateAt + 4 - kBrainDestAt)) return false;
    *state = static_cast<int>(R32(base, brain + kBrainStateAt));
    *dest = static_cast<int>(R32(base, brain + kBrainDestAt));
    return true;
}

// The name string of record `index` of the game's car table (a game-owned string: it stays), or 0.
uint32_t CarTableName(const uint8_t* base, int32_t index) {
    if (index < 0 || !Mapped(kCarTable, 8)) return 0;
    const uint32_t recs = R32(base, kCarTable);
    const uint32_t count = R16(base, kCarTable + 4);
    if (static_cast<uint32_t>(index) >= count || !Mapped(recs + kCarRecordBytes * static_cast<uint32_t>(index), 4)) return 0;
    return R32(base, recs + kCarRecordBytes * static_cast<uint32_t>(index));
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- the state

struct CarKey {
    double t = 0.0;
    int line = 0;
    CarCommand cmd;
    bool applied = false;  // its time has come and the Director was handed it
};

// A car the Director's `car.place` asked to be put somewhere, for the next update that may call natives.
struct Placement {
    std::string id;
    double position[3] = {};
    double heading = 0.0;  // the scene's convention
    bool has_heading = false;
};

// What the game thread knows of a paint seed held for an ambient car (the paint section below), for the log.
struct ForcedNote {
    std::string car;
    int32_t layout_id = -1;
    int paint = 0;
    uint32_t item = 0;
    uint32_t seed = 0;
    uint32_t hits_logged = 0;
    bool stands_warned = false;
    size_t slot = 0;
};

struct RouteEntry {
    std::shared_ptr<Route> route;
    std::string error;  // why there is none
};

// How a scene car's matrix reads, so that a car whose pose goes stale says why in the log (M4c's first run: a car's pose stopped updating a second into its
// path, the log said "the car lags" and nothing said the matrix was no longer taken).
struct ReadHealth {
    uint64_t failing_since = 0;  // the update the current run of refused reads began (0: the last read was taken)
    uint64_t refused = 0;        // refused reads of the scene so far
    int lines = 0;               // fault lines written for this car (capped)
    bool streak_logged = false;  // the current run of refused reads was written
    bool w_noted = false;        // the "w words are garbage" line was written
    bool ever_taken = false;     // a read was taken: only then is a refusal news (a car that is still arriving has no entity for a second after Racer_Activate; one that never reads at all
                                 // is the 'placed, but the pose cannot be read' line's)
};
constexpr int kFaultLinesPerCar = 6;
constexpr uint64_t kFaultAfterUpdates = 30;  // a run of refused reads is written once it is half a second old

struct State {
    std::recursive_mutex mutex;  // the camera hook reads poses; both run on the game thread (a native could even call the hook inside TickCars)
    Director dir;
    std::vector<CarKey> keys;
    std::vector<Placement> placements;
    uint64_t frame = 0;
    int ready_ticks = 0;
    int64_t next_pose_log_ms = 0;
    bool was_ready = false;
    std::map<std::string, RouteEntry> routes;  // the route files of the scene's path keys, by the name as written
    double path_clock = 0.0;                   // the scene's time with its time scale applied, in seconds
    double last_scene_t = 0.0;
    bool have_scene_t = false;
    std::map<std::string, ReadHealth> reads;  // how each scene car's matrix reads (for the log only)
    uint32_t ambient_manager = 0;               // the ambient manager the item cache belongs to
    std::map<int32_t, uint32_t> ambient_items;  // layout item id -> the item's address (found by a walk of the layout pool's lists, then kept while the id still reads there)
    std::vector<ForcedNote> forced;             // the ambient cars whose paint seed is held (the table of atomics is g_forced)
};
State g;
std::atomic<bool> g_active{false};  // anything to do: the one flag TickCars tests

void FlushEvents() {
    for (const CarEvent& e : g.dir.TakeEvents()) {
        if (e.warning) LARECOMP_APP_WARN("[cars] {}", e.text);
        else LARECOMP_APP_INFO("[cars] {}", e.text);
    }
}

// The one line a scene's cars end with: every car the scene made is deleted (or failed). Written by TickCars when the clean-up after the scene's end finished, or by the
// scene's end itself when the scene had deleted its cars already (TickCars no longer runs then: M4c's first run waited for a line that was never written).
void LogNothingLeft() {
    LARECOMP_APP_INFO("[cars] nothing left to do: {} car(s) made, {} deleted, {} failed", g.dir.spawned(), g.dir.deleted(), g.dir.failed());
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::string Triple(double x, double y, double z) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", x, y, z);
    return buf;
}

// ---------------------------------------------------------------- ambient cars (the layout)

bool PlausiblePtr(uint32_t p) {
    return p >= 0x10000u && p < 0xE0000000u;
}

// The layout's pool of the ambient manager `mgr`, or 0.
uint32_t LayoutPool(const uint8_t* base, uint32_t mgr) {
    if (!PlausiblePtr(mgr) || !Mapped(mgr + kMgrLayoutAt, 4)) return 0;
    const uint32_t data = R32(base, mgr + kMgrLayoutAt);
    if (!PlausiblePtr(data) || !Mapped(data + 32, 4)) return 0;
    const uint32_t pool = R32(base, data + 32);
    return PlausiblePtr(pool) && Mapped(pool, kPoolDescBytes) ? pool : 0;
}

// What the game side saw this update, for the guards of ambient_car.h.
AmbientGate ReadAmbientGate(const uint8_t* base, uint32_t player, bool paused, int ready_ticks) {
    AmbientGate gate;
    const uint32_t mgr = R32(base, kAmbientMgrPtr);
    gate.manager = PlausiblePtr(mgr) && Mapped(mgr, 16);
    gate.manager_addr = gate.manager ? mgr : 0;
    gate.player = player != 0;
    gate.free_roam = RpcInFreeRoam();
    gate.not_paused = !paused;
    gate.world_updates = ready_ticks > 0 ? static_cast<uint32_t>(ready_ticks) : 0;
    const uint32_t pool = gate.manager ? LayoutPool(base, mgr) : 0;
    if (pool) {
        const uint32_t free_slots = R32(base, pool + kDescFreeCount);
        if (free_slots < 100000) {  // a count that is not a count: the pool is not where it was taken to be
            gate.layout_pool = true;
            gate.layout_free = free_slots;
        }
    }
    return gate;
}

// A name the game keeps at `ptr`: printable ASCII up to a NUL, 47 characters at the most; "" when it is not a name.
std::string GuestName(const uint8_t* base, uint32_t ptr) {
    if (!PlausiblePtr(ptr)) return std::string();
    for (uint32_t span : {48u, 24u, 12u}) {  // a name at the end of a mapped region: a shorter span
        if (!Mapped(ptr, span)) continue;
        std::string s;
        for (uint32_t i = 0; i < span; ++i) {
            const uint8_t ch = base[ptr + i];
            if (ch == 0) return s;
            if (ch <= 0x20 || ch >= 0x7F) return std::string();
            s.push_back(static_cast<char>(ch));
        }
        return std::string();
    }
    return std::string();
}

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

// The game's own spelling of an ambient type's name (the manager's full type list: [mgr+4] -> [+0] the first type, a type's name pointer at +172, its next link at +20 -> [+0]),
// or "" when the manager has no such type. The layout item's car is made from the type with this name.
std::string AmbientTypeSpelling(const uint8_t* base, uint32_t mgr, const std::string& model) {
    if (!PlausiblePtr(mgr) || !Mapped(mgr + kMgrTypeListAt, 4)) return std::string();
    const uint32_t head = R32(base, mgr + kMgrTypeListAt);
    if (!PlausiblePtr(head) || !Mapped(head, 4)) return std::string();
    uint32_t t = R32(base, head);
    for (uint32_t n = 0; PlausiblePtr(t) && n < kTypeCap && Mapped(t, kTypeNameAt + 4); ++n) {
        const std::string name = GuestName(base, R32(base, t + kTypeNameAt));
        if (!name.empty() && EqualNoCase(name, model)) return name;
        const uint32_t link = R32(base, t + kTypeNextAt);
        if (!PlausiblePtr(link) || !Mapped(link, 4)) break;
        t = R32(base, link);
    }
    return std::string();
}

// The layout item with this id, in the pool's used and live lists (the add puts it in the used list, the spawner moves it to the live list when it makes the car).
uint32_t FindLayoutItem(const uint8_t* base, uint32_t pool, int32_t id) {
    for (uint32_t head_at : {kDescUsedHead, kDescLiveHead}) {
        uint32_t node = R32(base, pool + head_at);
        for (uint32_t n = 0; PlausiblePtr(node) && n < kListCap && Mapped(node, 12); ++n) {
            const uint32_t item = R32(base, node);
            if (PlausiblePtr(item) && Mapped(item, kItemBytes) && static_cast<int32_t>(R32(base, item + kItemId)) == id) return item;
            node = R32(base, node + 4);
        }
    }
    return 0;
}

// The matrix of the car the layout item `id` made (rows right, up, back, position), or false: no item, no car bound yet (the spawner makes it within a second), or its memory
// does not read. The item's address is kept while its id still reads there.
bool ReadAmbientMatrix(const uint8_t* base, int32_t id, float out[16]) {
    const uint32_t mgr = R32(base, kAmbientMgrPtr);
    const uint32_t pool = LayoutPool(base, mgr);
    if (!pool) return false;
    uint32_t item = 0;
    const auto cached = g.ambient_items.find(id);
    if (cached != g.ambient_items.end() && Mapped(cached->second, kItemBytes) && static_cast<int32_t>(R32(base, cached->second + kItemId)) == id) item = cached->second;
    if (!item) {
        item = FindLayoutItem(base, pool, id);
        if (!item) return false;
        g.ambient_items[id] = item;
    }
    const uint32_t car = R32(base, item + kItemBound);
    if (!PlausiblePtr(car) || !Mapped(car, kAmbientCarBytes)) return false;
    for (uint32_t i = 0; i < 16; ++i) out[i] = RF(base, car + kAmbientMatrixAt + 4 * i);
    return true;
}

// ---------------------------------------------------------------- the paint: the seed of an ambient car's own generator (scene_seed.h)

// The scene's ambient cars that have a paint, by the layout item the game turns into the car. The hook MCLAAmbientCarSeed (the end of this file; larecomp_config.toml) runs
// on whichever thread the game's spawn queue is given out on (sub_826DF260), so the table is atomics only and the game thread's notes (g.forced) are kept apart.
constexpr size_t kForcedSlots = 8;
struct ForcedSlot {
    std::atomic<uint32_t> item{0};   // the layout item (guest address) whose car takes the seed; 0 = free
    std::atomic<uint32_t> seed{0};
    std::atomic<uint32_t> hits{0};   // queue entries the hook has changed for this item
};
ForcedSlot g_forced[kForcedSlots];
std::atomic<uint32_t> g_forced_live{0};     // slots in use: the hook's one load when no scene car has a paint
std::atomic<uint32_t> g_hook_entries{0};    // queue entries given out while a slot was in use (matched or not)
std::atomic<uint32_t> g_hook_last_item{0};  // the item word of the last entry that matched no slot (for the log: is entry + 560 an item at all?)

void ClearForcedSeed(int32_t layout_id) {
    for (auto it = g.forced.begin(); it != g.forced.end();) {
        if (it->layout_id == layout_id) {
            g_forced[it->slot].item.store(0, std::memory_order_release);
            g_forced_live.fetch_sub(1, std::memory_order_relaxed);
            it = g.forced.erase(it);
        } else {
            ++it;
        }
    }
}

void ClearAllForcedSeeds() {
    for (ForcedSlot& s : g_forced) s.item.store(0, std::memory_order_release);
    g_forced_live.store(0, std::memory_order_relaxed);
    g.forced.clear();
}

// After AmbientMgr_AddAmbientToLayout returned the layout item's id: the item's queue entry will carry the paint's seed instead of the game's random number.
void NoteAmbientPaint(const uint8_t* base, const std::string& car, const Call& c, int32_t layout_id) {
    if (layout_id < 0 || c.paint_seed == 0) return;
    const uint32_t pool = LayoutPool(base, R32(base, kAmbientMgrPtr));
    const uint32_t item = pool ? FindLayoutItem(base, pool, layout_id) : 0;
    if (!item) {
        LARECOMP_APP_WARN("[cars] {} paint {}: the layout item {} was not found in the pool right after the add: the car keeps the game's random paint", car, c.paint, layout_id);
        return;
    }
    for (size_t i = 0; i < kForcedSlots; ++i) {
        uint32_t none = 0;
        if (!g_forced[i].item.compare_exchange_strong(none, item, std::memory_order_acq_rel)) continue;
        g_forced[i].seed.store(c.paint_seed, std::memory_order_relaxed);
        g_forced[i].hits.store(0, std::memory_order_relaxed);
        g_forced_live.fetch_add(1, std::memory_order_relaxed);
        ForcedNote note;
        note.car = car;
        note.layout_id = layout_id;
        note.paint = c.paint;
        note.item = item;
        note.seed = c.paint_seed;
        note.slot = i;
        g.forced.push_back(note);
        LARECOMP_APP_INFO("[cars] {} paint {}: the car's own random seed will be 0x{:08X} instead of a draw (layout item {} at {}); the log says when the game's spawn queue gave it out", car,
                          c.paint, c.paint_seed, layout_id, Hex(item));
        return;
    }
    LARECOMP_APP_WARN("[cars] {} paint {}: no free slot for the seed ({} at most): the car keeps the game's random paint", car, c.paint, kForcedSlots);
}

// Once an update while a paint is held: says when the hook has fired for an item, and warns once when a car stands and the hook never did.
void LogForcedSeeds(const uint8_t* base) {
    if (g.forced.empty()) return;
    for (ForcedNote& n : g.forced) {
        const uint32_t hits = g_forced[n.slot].hits.load(std::memory_order_relaxed);
        if (hits != n.hits_logged) {
            n.hits_logged = hits;
            LARECOMP_APP_INFO("[cars] {} paint {}: the game's spawn queue gave out the car's request, the seed 0x{:08X} went in ({} time(s))", n.car, n.paint, n.seed, hits);
        }
        if (hits == 0 && !n.stands_warned) {
            const uint32_t car = Mapped(n.item + kItemBound, 4) ? R32(base, n.item + kItemBound) : 0;
            if (car != 0) {
                n.stands_warned = true;
                LARECOMP_APP_WARN("[cars] {} paint {}: the car stands (item {} is bound to car {}) and the seed hook never fired for it: its paint is the game's random one. The hook saw {} "
                                  "queue entr{}; the last item word it did not match was {} (CHECK: the entry + 560 layout; the car may be made before the add's result was read)",
                                  n.car, n.paint, Hex(n.item), Hex(car), g_hook_entries.load(), g_hook_entries.load() == 1 ? "y" : "ies", Hex(g_hook_last_item.load()));
            }
        }
    }
}

// The hook's work (MCLAAmbientCarSeed): `entry` is the queue entry (guest address) whose request is being given out, `seed` the 31-bit draw the game made for it and is about
// to store at entry + 832; the answer is the seed to store.
uint32_t AmbientQueueSeed(uint32_t entry, uint32_t seed) {
    if (g_forced_live.load(std::memory_order_relaxed) == 0) return seed;
    auto* rt = REX_RUNTIME();
    const uint8_t* base = rt ? rt->GetMembase().GetBase() : nullptr;
    if (!base || entry < 0x10000u || entry > 0xBFFFF000u) return seed;
    g_hook_entries.fetch_add(1, std::memory_order_relaxed);
    const uint32_t item = R32(base, entry + seed::kQueueEntryItemAt);
    for (ForcedSlot& s : g_forced) {
        if (item != 0 && s.item.load(std::memory_order_acquire) == item) {
            s.hits.fetch_add(1, std::memory_order_relaxed);
            return s.seed.load(std::memory_order_relaxed);
        }
    }
    g_hook_last_item.store(item, std::memory_order_relaxed);
    return seed;
}

// ---------------------------------------------------------------- the poses

std::string Words(const float m[16]) {
    std::string s;
    char buf[24];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(buf, sizeof(buf), "%s%g", i ? " " : "", static_cast<double>(m[i]));
        s += buf;
    }
    return s;
}

// One read of a scene car's matrix, for the log: the first refused read of a run says which step refused (the pointers of the chain, the words), the first
// read after it says the matrix is back, and a w word that is not finite is said once (the registry ignores it).
void NoteRead(const std::string& id, uint32_t racer, bool read, const MatrixRead& info, const float m[16]) {
    ReadHealth& h = g.reads[id];
    std::string why = info.stage;
    if (read) {
        why = MatrixFault(m);
        if (why.empty()) {
            if (h.failing_since != 0 && h.streak_logged) LARECOMP_APP_INFO("[cars] {} the matrix reads again after {} update(s)", id, g.frame - h.failing_since);
            h.failing_since = 0;
            h.streak_logged = false;
            h.ever_taken = true;
            if (MatrixHasBadW(m) && !h.w_noted) {
                h.w_noted = true;
                LARECOMP_APP_INFO("[cars] {} a w word of the matrix is not finite (ignored: Racer_Teleport leaves the position row's w from the stack): {}", id, Words(m));
            }
            return;
        }
        why += "; words " + Words(m);
    }
    ++h.refused;
    if (h.failing_since == 0) h.failing_since = g.frame;
    if (!h.ever_taken || h.streak_logged || g.frame - h.failing_since < kFaultAfterUpdates || h.lines >= kFaultLinesPerCar) return;
    h.streak_logged = true;
    ++h.lines;
    LARECOMP_APP_WARN("[cars] {} the matrix has not been taken for {} updates (racer {}, entity {}, matrix {}): {}", id, g.frame - h.failing_since, Hex(racer), Hex(info.entity),
                      Hex(info.matrix), why);
}

// Reads every car that exists into the registry: the scene's cars that have streamed in, and the player.
void UpdatePoses(const uint8_t* base, uint32_t player) {
    CarRegistry& reg = g.dir.registry();
    for (const Director::Reader& r : g.dir.PoseReaders()) {
        CarEntry* e = reg.Find(r.id);
        if (!e) continue;
        e->racer = r.racer;
        float m[16];
        MatrixRead info;
        const bool read = ReadCarMatrix(base, r.racer, m, &info);
        NoteRead(r.id, r.racer, read, info, m);
        if (read) reg.UpdatePose(r.id, m, g.frame);
    }
    for (const Director::AmbientReader& r : g.dir.AmbientReaders()) {
        float m[16];
        if (ReadAmbientMatrix(base, r.layout_id, m)) g.dir.NoteAmbientPose(r.id, m, g.frame);
    }
    if (CarEntry* pe = reg.Find("player")) {
        pe->racer = player;
        float m[16];
        if (player && ReadCarMatrix(base, player, m)) reg.UpdatePose("player", m, g.frame);
    }
}

void LogPoses(int64_t now) {
    if (now < g.next_pose_log_ms) return;
    g.next_pose_log_ms = now + kPoseLogEveryMs;
    for (const CarEntry& e : g.dir.registry().entries()) {
        if (!e.have_pose) continue;
        LARECOMP_APP_INFO("[cars] pose {} {} heading {:.1f} (racer {}, matrix read {} update(s) ago)", e.id,
                          Triple(e.pose.position[0], e.pose.position[1], e.pose.position[2]), e.pose.heading, Hex(e.racer),
                          g.frame - e.pose_frame);
    }
}

// The matrix of `id` as the game stores it (the live one when it can be read, else the last good one), and its pose.
bool PoseOf(std::string_view id, float matrix[16], CarPose* pose) {
    uint8_t* base = Membase();
    if (!base) return false;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    CarEntry* e = g.dir.registry().Find(id);
    uint32_t racer = 0;
    if (id == "player") racer = PlayerRacer(base);
    else if (e) racer = e->racer;
    else return false;
    float m[16];
    CarPose p;
    if (racer && ReadCarMatrix(base, racer, m) && PoseFromMatrix(m, &p)) {
        if (e) g.dir.registry().UpdatePose(id, m, g.frame);
        if (matrix) {
            std::memcpy(matrix, m, sizeof(m));
            matrix[3] = matrix[7] = matrix[11] = 0.0f;  // the w words may be garbage (see PoseFromMatrix): the camera is given clean ones
            matrix[15] = 1.0f;
        }
        if (pose) *pose = p;
        return true;
    }
    if (e && e->have_pose) {
        if (matrix) std::memcpy(matrix, e->matrix, sizeof(e->matrix));
        if (pose) *pose = e->pose;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the calls

bool TakesRacer(Native n) {
    switch (n) {
        case Native::kLoadOpponent:
        case Native::kLoadOpponentByConfig:
        case Native::kAddOpponent:
        case Native::kGetLoadState:
        case Native::kIsCarStreamed:
        case Native::kActivate:
        case Native::kLockStreaming:
        case Native::kTeleport:
        case Native::kGetPosition:
        case Native::kDeleteOpponent:
        case Native::kChangeCarByConfig:
        case Native::kCreateBrain:
        case Native::kBrainSetSpeed:
        case Native::kBrainSetDestSpeed:
        case Native::kBrainSetLooping:
        case Native::kBrainAppendDest:
        case Native::kBrainDrive:
        case Native::kBrainReached:
        case Native::kBrainStop:
        case Native::kDeleteBrain:
            return true;  // (the brain natives take the brain, and the car it drives is the racer the books are checked for)
        default:
            return false;
    }
}

bool TakesBrain(Native n) {
    switch (n) {
        case Native::kBrainSetSpeed:
        case Native::kBrainSetDestSpeed:
        case Native::kBrainSetLooping:
        case Native::kBrainAppendDest:
        case Native::kBrainDrive:
        case Native::kBrainReached:
        case Native::kBrainStop:
            return true;
        default:
            return false;
    }
}

// The natives that are only called on a racer already in the opponent manager's active list.
bool NeedsInWorld(Native n) {
    switch (n) {
        case Native::kGetLoadState:
        case Native::kIsCarStreamed:
        case Native::kActivate:
        case Native::kLockStreaming:
        case Native::kTeleport:
        case Native::kGetPosition:
        case Native::kDeleteOpponent:
        case Native::kCreateBrain:
        case Native::kBrainSetSpeed:
        case Native::kBrainSetDestSpeed:
        case Native::kBrainSetLooping:
        case Native::kBrainAppendDest:
        case Native::kBrainDrive:
        case Native::kBrainReached:
        case Native::kBrainStop:
        case Native::kDeleteBrain:
            return true;
        default:
            return false;
    }
}

bool IsOpponentManagerNative(Native n) {
    return n == Native::kCreateOpponent || n == Native::kLoadOpponent || n == Native::kLoadOpponentByConfig || n == Native::kAddOpponent ||
           n == Native::kDeleteOpponent;
}

std::string Describe(const Call& c) {
    std::string s;
    if (c.racer) s += " racer " + Hex(c.racer);
    if (!c.text.empty()) s += " \"" + c.text + "\"";
    if (c.ptr) s += " ptr " + Hex(c.ptr);
    if (c.brain) s += " brain " + Hex(c.brain);
    if (c.native == Native::kBrainSetSpeed || c.native == Native::kBrainSetDestSpeed) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %.2f m/s", c.speed);
        s += buf;
    }
    if (c.native == Native::kBrainAppendDest) s += " #" + std::to_string(c.index) + " at " + Triple(c.position[0], c.position[1], c.position[2]);
    if (c.native == Native::kGetProfile || c.native == Native::kHostCarName) s += " index " + std::to_string(c.index);
    if (c.native == Native::kRemoveAmbient) s += " layout item " + std::to_string(c.index);
    if (c.paint >= 0) s += " paint " + std::to_string(c.paint);
    if (c.native == Native::kLoadOpponent || c.native == Native::kLoadOpponentByConfig || c.native == Native::kTeleport || c.native == Native::kAddAmbient) {
        s += " at " + Triple(c.position[0], c.position[1], c.position[2]) + " heading " + std::to_string(static_cast<int>(std::lround(c.heading_native))) + " (native)";
        if (c.native == Native::kTeleport && c.speed != 0.0) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " speed %.2f m/s", c.speed);
            s += buf;
        }
    }
    return s;
}

// After Racer_GetPosition: the native's answer, the matrix's, and what the scene asked for.
void LogPlacement(const uint8_t* base, const Director::Next& n, const NativeResult& r) {
    float m[16];
    CarPose p;
    const bool have = ReadCarMatrix(base, n.call.racer, m) && PoseFromMatrix(m, &p);
    const double ax = n.call.position[0], ay = n.call.position[1], az = n.call.position[2];
    const double asked_heading = NormalizeDegrees(n.call.heading_native - 180.0);  // the scene's heading: the native's angle minus 180
    if (!have || r.vec_count == 0) {
        LARECOMP_APP_WARN("[cars] {} placed, but the pose cannot be read: the matrix {}, Racer_GetPosition {}", n.car, have ? "reads" : "does not read",
                          r.vec_count ? "answered" : "gave no vector");
        return;
    }
    const double nx = r.vec[0][0], ny = r.vec[0][1], nz = r.vec[0][2];
    const double dn = std::sqrt((nx - p.position[0]) * (nx - p.position[0]) + (ny - p.position[1]) * (ny - p.position[1]) +
                                (nz - p.position[2]) * (nz - p.position[2]));
    const double da = std::sqrt((ax - p.position[0]) * (ax - p.position[0]) + (az - p.position[2]) * (az - p.position[2]));
    double dh = std::fabs(p.heading - asked_heading);
    if (dh > 180.0) dh = 360.0 - dh;
    LARECOMP_APP_INFO("[cars] {} placed: the matrix says {} heading {:.1f}; Racer_GetPosition says {} ({:.2f} m apart); the scene asked for {} heading {:.1f} "
                      "(off by {:.2f} m on the ground, {:.1f} degrees)",
                      n.car, Triple(p.position[0], p.position[1], p.position[2]), p.heading, Triple(nx, ny, nz), dn, Triple(ax, ay, az),
                      asked_heading, da, dh);
}

// Makes the call the Director handed out and reports to it. Returns after one native at most.
void Execute(const uint8_t* base, const Director::Next& n, uint32_t mgr) {
    const Call& c = n.call;
    Outcome out;

    if (c.native == Native::kHostCarName) {  // not a native: the host reads the game's own name string
        out.called = true;
        out.ret_written = true;
        out.ret = CarTableName(base, c.index);
        LARECOMP_APP_INFO("[cars] {} car table record {} -> name at {}", n.car, c.index, Hex(out.ret));
        g.dir.Report(out);
        return;
    }
    const uint32_t wrapper = WrapperFor(c.native);
    if (!wrapper) {
        LARECOMP_APP_ERROR("[cars] {} {}: no wrapper address for this native", n.car, NativeName(c.native));
        g.dir.Report(out);  // not called
        return;
    }
    if (TakesRacer(c.native) && c.racer == 0) {
        LARECOMP_APP_ERROR("[cars] {} {}: refused, the racer is 0", n.car, NativeName(c.native));
        g.dir.Report(out);
        return;
    }
    if (TakesBrain(c.native) && c.brain == 0) {
        LARECOMP_APP_ERROR("[cars] {} {}: refused, the brain is 0", n.car, NativeName(c.native));
        g.dir.Report(out);
        return;
    }
    if (IsOpponentManagerNative(c.native) && !mgr) {
        LARECOMP_APP_ERROR("[cars] {} {}: refused, the opponent manager does not exist", n.car, NativeName(c.native));
        g.dir.Report(out);
        return;
    }
    if (NeedsInWorld(c.native) && !InManager(base, mgr, c.racer)) {
        LARECOMP_APP_WARN("[cars] {} {}: racer {} is not in the opponent manager's list any more (the world was rebuilt?): the car is dropped, nothing is called",
                          n.car, NativeName(c.native), Hex(c.racer));
        g.dir.MarkLost(c.racer);
        g.dir.CancelPending();
        return;
    }

    std::vector<uint32_t> args;
    const uint32_t zero_bits = std::bit_cast<uint32_t>(0.0f);
    const uint32_t heading_bits = std::bit_cast<uint32_t>(static_cast<float>(c.heading_native));
    const uint32_t speed_bits = std::bit_cast<uint32_t>(static_cast<float>(c.speed));
    switch (c.native) {
        case Native::kLookupCar:
            args = {GuestPutString(c.text.c_str())};
            break;
        case Native::kAddAmbient: {
            // The model must be one of the ambient manager's types (the name goes to the native in the game's own spelling) and the point near the player: the probe's checks.
            const std::string spelled = AmbientTypeSpelling(base, R32(base, kAmbientMgrPtr), c.text);
            if (spelled.empty()) {
                LARECOMP_APP_ERROR("[cars] {} {}: '{}' is not in the ambient manager's type list (the traffic probe's `types:` lines name them, e.g. va_gmc_yukon_05): not placed", n.car,
                                   NativeName(c.native), c.text);
                g.dir.Report(out);  // not called
                return;
            }
            if (const CarEntry* pe = g.dir.registry().Find("player"); pe && pe->have_pose) {
                const double dx = c.position[0] - pe->pose.position[0], dy = c.position[1] - pe->pose.position[1], dz = c.position[2] - pe->pose.position[2];
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > kAmbientMaxFromPlayerM) {
                    LARECOMP_APP_ERROR("[cars] {} {}: the point {} is more than {:.0f} m from the player's car: not placed (the layout is read around the player)", n.car,
                                       NativeName(c.native), Triple(c.position[0], c.position[1], c.position[2]), kAmbientMaxFromPlayerM);
                    g.dir.Report(out);  // not called
                    return;
                }
            }
            args = {GuestPutString(spelled.c_str()),
                    GuestPutVec(static_cast<float>(c.position[0]), static_cast<float>(c.position[1]), static_cast<float>(c.position[2]), 0.0f), heading_bits};
            break;
        }
        case Native::kRemoveAmbient:
            args = {static_cast<uint32_t>(c.index)};
            break;
        case Native::kGetProfile:
            args = {static_cast<uint32_t>(c.index)};
            break;
        case Native::kLoadOpponent:
        case Native::kLoadOpponentByConfig:
            args = {c.racer, c.ptr,
                    GuestPutVec(static_cast<float>(c.position[0]), static_cast<float>(c.position[1]), static_cast<float>(c.position[2]), 0.0f),
                    heading_bits};
            break;
        case Native::kTeleport:
            args = {c.racer,
                    GuestPutVec(static_cast<float>(c.position[0]), static_cast<float>(c.position[1]), static_cast<float>(c.position[2]), 0.0f),
                    heading_bits, speed_bits};
            break;
        case Native::kGetPosition:
            args = {c.racer, GuestPutVec(0.0f, 0.0f, 0.0f, 0.0f)};
            break;
        case Native::kChangeCarByConfig:
            args = {c.racer, c.ptr};
            break;
        case Native::kBrainSetSpeed:
        case Native::kBrainSetDestSpeed:
            args = {c.brain, speed_bits};
            break;
        case Native::kBrainSetLooping:
            args = {c.brain, c.index != 0 ? 1u : 0u};
            break;
        case Native::kBrainAppendDest:
            args = {c.brain, GuestPutVec(static_cast<float>(c.position[0]), static_cast<float>(c.position[1]), static_cast<float>(c.position[2]), 0.0f)};
            break;
        case Native::kBrainDrive:
        case Native::kBrainReached:
        case Native::kBrainStop:
            args = {c.brain};
            break;
        case Native::kAddOpponent:
        case Native::kGetLoadState:
        case Native::kIsCarStreamed:
        case Native::kActivate:
        case Native::kLockStreaming:
        case Native::kDeleteOpponent:
        case Native::kCreateBrain:
        case Native::kDeleteBrain:
            args = {c.racer};
            break;
        default:  // no arguments: the garage counts, CreateOpponent
            break;
    }
    // a pointer argument the shim could not make (no block): not called
    for (size_t i = 0; i < args.size(); ++i) {
        const bool pointer = (c.native == Native::kLookupCar && i == 0) || (c.native == Native::kAddAmbient && (i == 0 || i == 1)) || ((c.native == Native::kLoadOpponent || c.native == Native::kLoadOpponentByConfig) && i == 2) ||
                             (c.native == Native::kTeleport && i == 1) || (c.native == Native::kGetPosition && i == 1) || (c.native == Native::kBrainAppendDest && i == 1);
        if (pointer && args[i] == 0) {
            LARECOMP_APP_ERROR("[cars] {} {}: no guest memory for the argument (no guest thread, or the call block could not be made)", n.car, NativeName(c.native));
            g.dir.Report(out);
            return;
        }
    }

    LARECOMP_APP_INFO("[cars] {} {}{}", n.car, NativeName(c.native), Describe(c));
    // A racer-class car's natives that may draw its look run with the game's generators held at the paint's seed (scene_seed.h); what was there comes back after.
    const bool draws_look = c.native == Native::kCreateOpponent || c.native == Native::kLoadOpponent || c.native == Native::kLoadOpponentByConfig || c.native == Native::kAddOpponent;
    const GeneratorHold hold(draws_look && c.paint >= 0 ? c.paint_seed : 0u);
    NativeResult r;
    switch (args.size()) {
        case 0: r = CallNative(wrapper, {}); break;
        case 1: r = CallNative(wrapper, {args[0]}); break;
        case 2: r = CallNative(wrapper, {args[0], args[1]}); break;
        case 3: r = CallNative(wrapper, {args[0], args[1], args[2]}); break;
        default: r = CallNative(wrapper, {args[0], args[1], args[2], args[3]}); break;
    }
    out.called = r.called;
    out.ret_written = r.ret_written;
    out.ret = r.ret;
    if (!r.called) {
        LARECOMP_APP_ERROR("[cars] {} {} (wrapper 0x{:08X}) could not be called (no guest thread, no such function, or no block for the context)", n.car,
                           NativeName(c.native), wrapper);
    } else {
        std::string value = r.ret_written ? Hex(r.ret) + " (int " + std::to_string(static_cast<int32_t>(r.ret)) + ")" : std::string("(no value)");
        if (r.vec_count > 0) value += "; vec0=" + Triple(r.vec[0][0], r.vec[0][1], r.vec[0][2]);
        LARECOMP_APP_INFO("[cars] {} {} -> {} ({:.3f} ms)", n.car, NativeName(c.native), value, r.ms);
        if (c.native == Native::kGetPosition) LogPlacement(base, n, r);
        if (c.native == Native::kAddAmbient && r.ret_written) NoteAmbientPaint(base, n.car, c, static_cast<int32_t>(r.ret));
        if (c.native == Native::kRemoveAmbient) ClearForcedSeed(c.index);
    }
    g.dir.Report(out);
}

// ---------------------------------------------------------------- the route files

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// The route a `path` key names, looked for beside the scene file and then in a `routes` folder beside it; a name with no extension also tries
// `.route`. The name was checked by the parser (a relative path, no `..`).
bool FindRouteFile(const std::string& name, std::filesystem::path* found, std::string* tried) {
    const std::string scene = SceneFilePath();
    std::filesystem::path dir = scene.empty() ? std::filesystem::path("scenes") : PathFromUtf8(scene).parent_path();
    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::path& base : {dir, dir / "routes"}) {
        candidates.push_back(base / PathFromUtf8(name));
        if (!PathFromUtf8(name).has_extension()) candidates.push_back(base / PathFromUtf8(name + ".route"));
    }
    std::error_code ec;
    for (const std::filesystem::path& c : candidates) {
        if (std::filesystem::is_regular_file(c, ec)) {
            *found = c;
            return true;
        }
        const std::u8string u = c.u8string();
        *tried += (tried->empty() ? "" : ", ") + std::string(u.begin(), u.end());
    }
    return false;
}

void LoadRoute(const std::string& name, int line) {
    if (g.routes.count(name)) return;
    RouteEntry entry;
    std::filesystem::path file;
    std::string tried;
    if (!FindRouteFile(name, &file, &tried)) {
        entry.error = "no route file '" + name + "' (looked for " + tried + ")";
    } else {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(file, ec);
        if (!ec && size > 4u * 1024 * 1024) {
            entry.error = "the route file is " + std::to_string(size / 1024) + " KB: too big for a route";
        } else {
            std::ifstream in(file, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto route = std::make_shared<Route>();
            std::string why;
            PathSampler probe;
            if (!ParseRoute(text, route.get(), &why) || !probe.Build(*route, SpeedSpec{}, &why)) {
                entry.error = why;
            } else {
                entry.route = route;
                const std::u8string u = file.u8string();
                LARECOMP_APP_INFO("[cars] route {} = {}: {} points, {:.1f} m{}", name, std::string(u.begin(), u.end()), probe.PointCount(), probe.Length(),
                                  route->has_y ? ", with heights" : "");
            }
        }
    }
    if (!entry.route) LARECOMP_APP_ERROR("[cars] line {}: route '{}': {}", line, name, entry.error);
    g.routes[name] = std::move(entry);
}

// ---------------------------------------------------------------- the paths

// Every update, for the cars that follow a route: the kinematic ones are put on it (one Racer_Teleport each), the brains are read. Not subject to the
// spawn steps' one native an update: these are the same cheap call E4 measured at 0.02 to 0.19 ms, once for each such car.
void TickPaths(const uint8_t* base, uint32_t mgr) {
    std::vector<Director::TeleportDue> due;
    g.dir.PollTeleports(g.path_clock, &due, g.frame);
    const uint32_t wrapper = WrapperFor(Native::kTeleport);
    for (const Director::TeleportDue& d : due) {
        const PathDrive::Teleport& t = d.teleport;
        if (!InManager(base, mgr, d.racer)) {
            LARECOMP_APP_WARN("[cars] {}: racer {} is not in the opponent manager's list any more (the world was rebuilt?): the car is dropped", d.car, Hex(d.racer));
            g.dir.MarkLost(d.racer);
            continue;
        }
        const uint32_t vec = GuestPutVec(static_cast<float>(t.position[0]), static_cast<float>(t.position[1]), static_cast<float>(t.position[2]), 0.0f);
        if (!vec) {
            LARECOMP_APP_ERROR("[cars] {} path put: no guest memory for Racer_Teleport's argument (no guest thread, or the call block could not be made)", d.car);
            g.dir.ReportTeleport(d.car, false);
            continue;
        }
        const bool noisy = t.first || t.last;
        if (noisy) {
            LARECOMP_APP_INFO("[cars] {} path put ({}): Racer_Teleport racer {} at {} heading {} (native) speed {:.2f} m/s, {:.1f} m along the route", d.car,
                              t.first ? "first" : "last", Hex(d.racer), Triple(t.position[0], t.position[1], t.position[2]),
                              static_cast<int>(std::lround(t.heading_native)), t.speed, t.along);
        }
        const NativeResult r = CallNative(wrapper, {d.racer, vec, std::bit_cast<uint32_t>(static_cast<float>(t.heading_native)),
                                                    std::bit_cast<uint32_t>(static_cast<float>(t.speed))},
                                          noisy);
        if (!r.called) {
            LARECOMP_APP_ERROR("[cars] {} path put: Racer_Teleport (wrapper 0x{:08X}) could not be called (no guest thread, no such function, or no block for the context)", d.car,
                               wrapper);
        }
        g.dir.ReportTeleport(d.car, r.called);
    }
    for (const Director::BrainReader& b : g.dir.BrainReaders()) {
        int state = 0;
        int dest = 0;
        if (ReadBrain(base, b.brain, &state, &dest)) g.dir.NoteBrain(b.id, g.path_clock, state, dest, g.frame);
    }
}

// The Director's `car.place`: one Racer_Teleport for each car asked for, at an update that may call natives. The car's own speed is 0 afterwards.
void TickPlacements(const uint8_t* base, uint32_t mgr, uint32_t player) {
    if (g.placements.empty()) return;
    std::vector<Placement> todo;
    todo.swap(g.placements);
    const uint32_t wrapper = WrapperFor(Native::kTeleport);
    for (const Placement& p : todo) {
        uint32_t racer = 0;
        CarEntry* e = g.dir.registry().Find(p.id);
        if (p.id == "player") racer = player;
        else if (e) racer = e->racer;
        if (!racer) {
            LARECOMP_APP_WARN("[cars] place {}: the car has no racer (not made yet, or gone)", p.id);
            continue;
        }
        if (p.id != "player" && !InManager(base, mgr, racer)) {
            LARECOMP_APP_WARN("[cars] place {}: racer {} is not in the opponent manager's list any more: nothing is called", p.id, Hex(racer));
            g.dir.MarkLost(racer);
            continue;
        }
        const double heading = p.has_heading ? p.heading : (e && e->have_pose ? e->pose.heading : 0.0);
        const uint32_t vec = GuestPutVec(static_cast<float>(p.position[0]), static_cast<float>(p.position[1]), static_cast<float>(p.position[2]), 0.0f);
        if (!vec) {
            LARECOMP_APP_ERROR("[cars] place {}: no guest memory for Racer_Teleport's argument", p.id);
            continue;
        }
        const NativeResult r = CallNative(wrapper, {racer, vec, std::bit_cast<uint32_t>(static_cast<float>(NativeHeadingDeg(heading))), std::bit_cast<uint32_t>(0.0f)}, false);
        LARECOMP_APP_INFO("[cars] place {}: Racer_Teleport racer {} at {} heading {:.1f}{}", p.id, Hex(racer), Triple(p.position[0], p.position[1], p.position[2]), heading,
                          r.called ? "" : " (the native could not be called)");
    }
}

// ---------------------------------------------------------------- the scene runner's applier

#if defined(LARECOMP_CARS_HAS_STATE_LOG)
// The state log's view of the scene's cars (state_log.h): the cars of the registry that a scene made, in the order they were added, the
// player not among them (the log has columns of its own for the player's car). Read on the game thread once an update while a log is open.
// Registered when a scene with car keys begins, not from a static constructor: the log's own state is made in another translation unit.
size_t StateLogCarCount() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    size_t n = 0;
    for (const CarEntry& e : g.dir.registry().entries()) {
        if (!e.is_player) ++n;
    }
    return n;
}

bool StateLogCarGet(size_t index, char* id, size_t id_size, float matrix[16]) {
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lock(g.mutex);
        size_t k = 0;
        for (const CarEntry& e : g.dir.registry().entries()) {
            if (e.is_player) continue;
            if (k++ == index) {
                name = e.id;
                break;
            }
        }
    }
    if (name.empty()) return false;
    if (id && id_size > 0) {  // the id is given even when the car has no pose this update
        const size_t n = std::min(name.size(), id_size - 1);
        std::memcpy(id, name.data(), n);
        id[n] = '\0';
    }
    return PoseOf(name, matrix, nullptr);
}

const statelog::CarSource kStateLogCars = {&StateLogCarCount, &StateLogCarGet};
#endif

// The `car` keys of a scene, in time order (the events are), each with its parse; a key the cars package does not know is reported and left out.
std::vector<CarKey> ReadCarKeys(const Scene& scene) {
    std::vector<CarKey> keys;
    for (const Event& e : scene.events) {
        if (e.track != Track::kCar) continue;
        CarCommand cmd;
        std::string why;
        switch (ParseCarCommand(e.words, &cmd, &why)) {
            case CarParse::kOk:
                keys.push_back({e.t, e.line, cmd, false});
                break;
            case CarParse::kOtherVerb:
                LARECOMP_APP_WARN("[cars] line {}: 'car {} {}' is not applied: the cars package knows spawn, ambient, delete, profile, path and stop", e.line,
                                  e.words.empty() ? std::string() : e.words[0], e.words.size() > 1 ? e.words[1] : std::string());
                break;
            case CarParse::kError:
                LARECOMP_APP_ERROR("[cars] line {}: {}", e.line, why);
                break;
        }
    }
    return keys;
}

void ApplierBegin(const Scene& scene) {
#if defined(LARECOMP_CARS_HAS_STATE_LOG)
    statelog::RegisterCarSource(&kStateLogCars);
#endif
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.keys.clear();
    g.routes.clear();
    g.path_clock = 0.0;
    g.last_scene_t = 0.0;
    g.have_scene_t = false;
    g.reads.clear();
    g.ambient_items.clear();
    ClearAllForcedSeeds();
    g.keys = ReadCarKeys(scene);
    if (g.keys.empty()) return;
    for (const CarKey& k : g.keys) {
        if (k.cmd.verb == CarVerb::kPath) LoadRoute(k.cmd.route, k.line);
    }
    g.dir.BeginScene();
    g_active.store(true, std::memory_order_relaxed);
    LARECOMP_APP_INFO("[cars] the scene '{}' has {} car key(s); one native an update, from {} updates after a car is on the road", scene.name, g.keys.size(), kSettleTicks);
}

void ApplierApply(const Scene& scene, double t) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    // The path clock: the scene's time, with the time scale the scene asked for at the last update applied to the step (the engine timer takes the
    // scale one update late, so a car follows the slowed-down world, not the scene's own wall time).
    if (!g.have_scene_t) {
        g.path_clock = t;
    } else if (std::fabs(t - g.last_scene_t) > 0.3) {
        // A jump (the Director's seek, a restart at a time): the clock is the world time the scene time stands for, the integral of the time scale track, so
        // that a car on a route stands where a run would have it (closed form of the time).
        g.path_clock = director::WorldTimeAt(scene, t);
    } else if (t != g.last_scene_t) {
        // Forwards, and backwards too: a still (mcla_scene_t, the test harness's `scene_at`) pins the scene clock to a time the running clock has just passed by
        // a fraction of a step, and a scrub goes back by more. The car must stand where its route has it at THAT scene time, so the clock follows both ways
        // (it used to follow only forwards, and a car then stood up to a step ahead of the pinned time). The scale is the one at the earlier of the two times.
        const WorldState w = EvaluateWorld(scene, std::min(t, g.last_scene_t));
        const double scale = w.timescale_set ? std::clamp(w.timescale, 0.0, 10.0) : 1.0;
        g.path_clock += (t - g.last_scene_t) * scale;
        if (g.path_clock < 0.0) g.path_clock = 0.0;
    }
    g.last_scene_t = t;
    g.have_scene_t = true;
    for (CarKey& k : g.keys) {
        if (k.t > t + 1e-9) break;  // the keys are in time order
        if (k.applied) continue;
        k.applied = true;
        const Route* route = nullptr;
        if (k.cmd.verb == CarVerb::kPath) {
            const auto it = g.routes.find(k.cmd.route);
            if (it != g.routes.end()) route = it->second.route.get();
        }
        // a key that came due a long way back (a seek over it, a car added to a scene that has run): the path it starts began at the key's own time
        const double key_clock = (t - k.t > 0.3) ? director::WorldTimeAt(scene, k.t) : g.path_clock;
        const Director::Applied a = g.dir.Apply(k.cmd, key_clock, route);
        if (a.ok) LARECOMP_APP_INFO("[cars] t={:.2f} (line {}) {}", t, k.line, a.message);
        else LARECOMP_APP_WARN("[cars] t={:.2f} (line {}) {}", t, k.line, a.message);
    }
}

void ApplierEnd() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    const bool had_cars = !g.keys.empty();
    g.keys.clear();
    g.dir.EndScene();
    ClearAllForcedSeeds();  // every car the scene placed exists by now or goes with its item: no queue entry of the scene is to be changed any more
    FlushEvents();
    const bool active = g.dir.Active();
    g_active.store(active, std::memory_order_relaxed);  // the cars it spawned are deleted over the next updates
    if (!active && had_cars) LogNothingLeft();           // ... unless the scene deleted them itself: nothing is left to wait for
}

bool SameVerbs(const CarCommand& a, const CarCommand& b) {
    return a.id == b.id && a.verb == b.verb && a.source == b.source && a.model == b.model && a.slot == b.slot && a.position[0] == b.position[0] &&
           a.position[1] == b.position[1] && a.position[2] == b.position[2] && a.has_heading == b.has_heading && a.heading == b.heading && a.route == b.route &&
           a.speed.kind == b.speed.kind && a.speed.value == b.speed.value && a.mode == b.mode && a.has_paint == b.has_paint && a.paint == b.paint;
}

bool SameKey(const CarKey& a, const CarKey& b) {
    return std::fabs(a.t - b.t) < 1e-9 && SameVerbs(a.cmd, b.cmd);
}

bool IsPathVerb(CarVerb v) {
    return v == CarVerb::kPath || v == CarVerb::kStop;
}

// The Director's soft reload (scene_runtime.h TrackApplier::reload): the file changed while the scene runs at time `t`. Per car: when the statements due by now
// are the same as before, nothing happens; when only its path statements differ, they are given to the car again; when anything else differs (the model, the
// place, a `delete`), the car is deleted and made again from the new statements. Keys that are not due yet wait for their time as usual.
void ApplierReload(const Scene& scene, double t) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    std::vector<CarKey> next = ReadCarKeys(scene);
    const bool had_keys = !g.keys.empty();
    std::vector<std::string> ids;
    const auto add_id = [&](const std::string& id) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    for (const CarKey& k : g.keys) add_id(k.cmd.id);
    for (const CarKey& k : next) add_id(k.cmd.id);
    for (const std::string& id : ids) {
        std::vector<const CarKey*> old_due, new_due;
        for (const CarKey& k : g.keys) {
            if (k.cmd.id == id && k.applied) old_due.push_back(&k);
        }
        for (const CarKey& k : next) {
            if (k.cmd.id == id && k.t <= t + 1e-9) new_due.push_back(&k);
        }
        bool same = old_due.size() == new_due.size();
        for (size_t i = 0; same && i < old_due.size(); ++i) same = SameKey(*old_due[i], *new_due[i]);
        if (same) {
            for (CarKey& k : next) {
                if (k.cmd.id == id && k.t <= t + 1e-9) k.applied = true;  // in force already
            }
            continue;
        }
        // Only the path statements differ: the car stays and is given them again. Otherwise it goes and is made again.
        std::vector<const CarKey*> old_rest, new_rest;
        for (const CarKey* k : old_due) {
            if (!IsPathVerb(k->cmd.verb)) old_rest.push_back(k);
        }
        for (const CarKey* k : new_due) {
            if (!IsPathVerb(k->cmd.verb)) new_rest.push_back(k);
        }
        bool rest_same = old_rest.size() == new_rest.size();
        for (size_t i = 0; rest_same && i < old_rest.size(); ++i) rest_same = SameKey(*old_rest[i], *new_rest[i]);
        const bool had_car = !old_due.empty();
        if (rest_same && had_car) {
            for (CarKey& k : next) {
                if (k.cmd.id == id && k.t <= t + 1e-9 && !IsPathVerb(k.cmd.verb)) k.applied = true;
                if (IsPathVerb(k.cmd.verb) && k.cmd.id == id && k.cmd.verb == CarVerb::kPath) g.routes.erase(k.cmd.route);  // the route file may have been written since
            }
            LARECOMP_APP_INFO("[cars] reload: car {} keeps its place, only its path statements changed: they are given to it again", id);
        } else if (had_car) {
            CarCommand del;
            del.id = id;
            del.verb = CarVerb::kDelete;
            const Director::Applied a = g.dir.Apply(del, g.path_clock, nullptr);
            LARECOMP_APP_INFO("[cars] reload: car {} changed: {}; it is made again from the new statements", id, a.message);
            for (CarKey& k : next) {
                if (IsPathVerb(k.cmd.verb) && k.cmd.id == id) g.routes.erase(k.cmd.route);
            }
        }
    }
    g.keys = std::move(next);
    for (const CarKey& k : g.keys) {
        if (k.cmd.verb == CarVerb::kPath) LoadRoute(k.cmd.route, k.line);
    }
    if (!g.keys.empty()) {
        if (!had_keys) g.dir.BeginScene();  // the scene had no cars until now (BeginScene also restarts the default paints' numbering, so not on every reload)
        g_active.store(true, std::memory_order_relaxed);
    }
}

const TrackApplier kApplier = {"cars", TrackBit(Track::kCar), &ApplierBegin, &ApplierApply, &ApplierEnd, &ApplierReload};

// The applier is registered when the program starts; the runner's own tables are constant-initialised, so the order of the translation
// units does not matter. When the camera package is in the build, the registry is its source for `car:<id>`.
struct Registrar {
    Registrar() {
        RegisterTrackApplier(&kApplier);
#if defined(LARECOMP_CARS_HAS_CAMERA_RUNTIME)
        SetCarResolver(&CarMatrixById);
#endif
    }
} g_registrar;

}  // namespace

// ---------------------------------------------------------------- the public side

void TickCars() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    uint8_t* base = Membase();
    if (!base) return;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    ++g.frame;
    const int64_t now = NowMs();

    const uint32_t player = PlayerRacer(base);
    const uint32_t mgr = R32(base, kOpponentMgrPtr);
    const bool world = player != 0 && mgr != 0 && R32(base, kAmbientMgrPtr) != 0 && !GamePaused(base);
    if (world) ++g.ready_ticks;
    else g.ready_ticks = 0;

    UpdatePoses(base, player);
    LogForcedSeeds(base);

    if (world && g.frame % kWorldCheckEvery == 0) {
        for (const Director::Reader& r : g.dir.InWorld()) {
            if (InManager(base, mgr, r.racer)) continue;
            LARECOMP_APP_WARN("[cars] {}: racer {} is not in the opponent manager's list any more (the world was rebuilt?): the car is dropped", r.id, Hex(r.racer));
            g.dir.MarkLost(r.racer);
        }
    }

    if (g.dir.HasAmbientCars()) {
        g.dir.SetAmbientGate(ReadAmbientGate(base, player, GamePaused(base), g.ready_ticks));
        // The ambient manager is rebuilt under the scene (a new world: its address changes): the placements went with the old one. The machine notices at the remove;
        // the item cache is of the old manager.
        if (g.ambient_manager != R32(base, kAmbientMgrPtr)) {
            g.ambient_manager = R32(base, kAmbientMgrPtr);
            g.ambient_items.clear();
        }
    }
    const bool calls_ok = world && g.ready_ticks >= kSettleTicks;
    if (calls_ok && !g.was_ready) LARECOMP_APP_INFO("[cars] the world is ready for native calls (a car on the road, not paused, the opponent manager at {})", Hex(mgr));
    g.was_ready = calls_ok;
    const Director::Next next = g.dir.Poll(now, player, calls_ok);
    if (next.call.native != Native::kNone) Execute(base, next, mgr);
    if (calls_ok) TickPaths(base, mgr);
    if (calls_ok) TickPlacements(base, mgr, player);

    FlushEvents();
    LogPoses(now);
    if (!g.dir.Active() && g.placements.empty()) {
        g_active.store(false, std::memory_order_relaxed);
        LogNothingLeft();
    }
}

bool GetCarPose(std::string_view id, CarPose* out) {
    CarPose p;
    if (!PoseOf(id, nullptr, &p)) return false;
    if (out) *out = p;
    return true;
}

bool CarMatrixById(const char* id, float matrix[16]) {
    if (!id || !matrix) return false;
    return PoseOf(id, matrix, nullptr);
}

size_t SceneCarCount() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    return g.dir.SceneCarCount();
}

// ---------------------------------------------------------------- what the Director reads and asks (cars.h)

namespace {
// The statement that made a car: the last applied `spawn`, `ambient` or `profile` key of the id.
std::string ModelOfCar(const std::string& id) {
    for (auto it = g.keys.rbegin(); it != g.keys.rend(); ++it) {
        if (!it->applied || it->cmd.id != id) continue;
        if (it->cmd.verb == CarVerb::kSpawn || it->cmd.verb == CarVerb::kAmbient) {
            return it->cmd.source == CarSource::kGarage ? "profile " + std::to_string(it->cmd.slot) : it->cmd.model;
        }
    }
    return id == "player" ? "player" : std::string();
}
std::string RouteOfCar(const std::string& id) {
    for (auto it = g.keys.rbegin(); it != g.keys.rend(); ++it) {
        if (it->applied && it->cmd.id == id && it->cmd.verb == CarVerb::kPath) return it->cmd.route;
    }
    return {};
}
}  // namespace

std::vector<SceneCarInfo> ListSceneCars() {
    std::vector<SceneCarInfo> out;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    bool have_player = false;
    for (const CarEntry& e : g.dir.registry().entries()) {
        SceneCarInfo i;
        i.id = e.id;
        i.model = ModelOfCar(e.id);
        i.ready = e.have_pose;
        if (e.have_pose) {
            i.position[0] = e.pose.position[0], i.position[1] = e.pose.position[1], i.position[2] = e.pose.position[2];
            i.heading = e.pose.heading;
        }
        const PathDrive* path = g.dir.PathOf(e.id);
        i.on_path = path && path->Active();
        i.brain = path && path->mode() == PathMode::kBrain;
        if (i.on_path) i.route = RouteOfCar(e.id);
        have_player = have_player || e.is_player;
        out.push_back(std::move(i));
    }
    if (!have_player) {  // no scene with cars: the player's car is still there
        CarPose p;
        if (PoseOf("player", nullptr, &p)) {
            SceneCarInfo i;
            i.id = "player";
            i.model = "player";
            i.ready = true;
            i.position[0] = p.position[0], i.position[1] = p.position[1], i.position[2] = p.position[2];
            i.heading = p.heading;
            out.insert(out.begin(), std::move(i));
        }
    }
    return out;
}

int PlaceCar(std::string_view id, const double position[3], double heading, bool has_heading) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    const CarEntry* e = g.dir.registry().Find(id);
    if (!e && id != "player") return 1;
    if (id != "player" && (!e->have_pose || e->racer == 0)) return 2;
    Placement p;
    p.id = std::string(id);
    p.position[0] = position[0], p.position[1] = position[1], p.position[2] = position[2];
    p.heading = heading;
    p.has_heading = has_heading;
    g.placements.erase(std::remove_if(g.placements.begin(), g.placements.end(), [&](const Placement& o) { return o.id == p.id; }), g.placements.end());
    g.placements.push_back(std::move(p));
    g_active.store(true, std::memory_order_relaxed);
    return 0;
}

std::vector<std::string> ListCarModels() {
    std::vector<std::string> names;
    const uint8_t* base = Membase();
    if (!base || !Mapped(kCarTable, 8)) return names;
    const uint32_t count = std::min<uint32_t>(R16(base, kCarTable + 4), 4000);
    for (uint32_t i = 0; i < count; ++i) {
        const std::string name = GuestName(base, CarTableName(base, static_cast<int32_t>(i)));
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

bool SceneCarsSettled(std::string* waiting_for) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (g.keys.empty()) return true;
    const auto wait = [&](const std::string& why) {
        if (waiting_for) *waiting_for = why;
        return false;
    };
    if (!g.have_scene_t) return wait("the scene's first update has not run");
    for (const CarKey& k : g.keys) {
        if (!k.applied && k.t <= g.last_scene_t + 1e-9) return wait("the car key of line " + std::to_string(k.line) + " is not given to the cars yet");
    }
    for (const CarEntry& e : g.dir.registry().entries()) {
        if (!e.is_player && !e.have_pose) return wait("car " + e.id + " is not ready (its pose is not readable yet)");
    }
    return true;
}

std::vector<SceneRouteInfo> ListSceneRoutes() {
    std::vector<SceneRouteInfo> out;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    for (const CarKey& k : g.keys) {
        if (k.cmd.verb != CarVerb::kPath) continue;
        SceneRouteInfo r;
        r.name = k.cmd.route;
        r.car = k.cmd.id;
        r.kinematic = k.cmd.mode == PathMode::kKinematic;
        r.line = k.line;
        const auto it = g.routes.find(k.cmd.route);
        if (it != g.routes.end() && it->second.route) {
            for (const RoutePoint& p : it->second.route->points) {
                r.points.push_back({p.x, p.has_y ? p.y : 0.0, p.z, p.speed < 0.0 ? kDefaultRouteSpeed : p.speed});
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace mc::cinematic

// An ambient car's own random seed (the paint, scene_seed.h). Midasm hook BEFORE the instruction at 0x826DF2DC (`stw r3,832(r31)` in sub_826DF260, the queue's give-out of a spawn
// request): r3 is the 31-bit number the game drew from its generator for the car, r31 the queue entry. For the entry of one of the scene's layout items the number becomes the
// paint's seed. One atomic load when no scene car has a paint.
void MCLAAmbientCarSeed(PPCRegister& r3, PPCRegister& r31) {
    r3.u64 = mc::cinematic::AmbientQueueSeed(static_cast<uint32_t>(r31.u64), static_cast<uint32_t>(r3.u64));
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>

#include "cars.h"

void MCLAAmbientCarSeed(PPCRegister&, PPCRegister&) {}

namespace mc::cinematic {
void TickCars() {}
bool GetCarPose(std::string_view, CarPose*) { return false; }
bool CarMatrixById(const char*, float[16]) { return false; }
size_t SceneCarCount() { return 0; }
std::vector<SceneCarInfo> ListSceneCars() { return {}; }
int PlaceCar(std::string_view, const double[3], double, bool) { return 1; }
std::vector<std::string> ListCarModels() { return {}; }
bool SceneCarsSettled(std::string*) { return true; }
std::vector<SceneRouteInfo> ListSceneRoutes() { return {}; }
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
