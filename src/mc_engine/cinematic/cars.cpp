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
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

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
#include "larecomp_log.h"
#include "native_call.h"
#include "scene_runtime.h"

#if __has_include("camera_runtime.h")
#include "camera_runtime.h"  // M1b: SetCarResolver. Present once its branch is merged; then the registry is the camera's car source
#define LARECOMP_CARS_HAS_CAMERA_RUNTIME 1
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
};

uint32_t WrapperFor(Native n) {
    for (const NativeDef& d : kNatives) {
        if (d.native == n) return d.wrapper;
    }
    return 0;
}

// ---------------------------------------------------------------- guest memory

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
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
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;  // the test map_warp.cpp uses
}

// The player's racer (0 in a menu, the garage, a loading screen, or off a guest thread).
uint32_t PlayerRacer(const uint8_t* base) {
    if (!rex::runtime::ThreadState::Get()) return 0;
    const uint32_t mgr = R32(base, kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(racer_fn, mgr, i)) return r;
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
};
constexpr int kFaultLinesPerCar = 6;
constexpr uint64_t kFaultAfterUpdates = 30;  // a run of refused reads is written once it is half a second old (a car that is still arriving has no entity yet)

struct State {
    std::recursive_mutex mutex;  // the camera hook reads poses; both run on the game thread (a native could even call the hook inside TickCars)
    Director dir;
    std::vector<CarKey> keys;
    size_t next_key = 0;
    uint64_t frame = 0;
    int ready_ticks = 0;
    int64_t next_pose_log_ms = 0;
    bool was_ready = false;
    std::map<std::string, RouteEntry> routes;  // the route files of the scene's path keys, by the name as written
    double path_clock = 0.0;                   // the scene's time with its time scale applied, in seconds
    double last_scene_t = 0.0;
    bool have_scene_t = false;
    std::map<std::string, ReadHealth> reads;  // how each scene car's matrix reads (for the log only)
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
    if (h.streak_logged || g.frame - h.failing_since < kFaultAfterUpdates || h.lines >= kFaultLinesPerCar) return;
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
    if (c.native == Native::kLoadOpponent || c.native == Native::kLoadOpponentByConfig || c.native == Native::kTeleport) {
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
        const bool pointer = (c.native == Native::kLookupCar && i == 0) || ((c.native == Native::kLoadOpponent || c.native == Native::kLoadOpponentByConfig) && i == 2) ||
                             (c.native == Native::kTeleport && i == 1) || (c.native == Native::kGetPosition && i == 1) || (c.native == Native::kBrainAppendDest && i == 1);
        if (pointer && args[i] == 0) {
            LARECOMP_APP_ERROR("[cars] {} {}: no guest memory for the argument (no guest thread, or the call block could not be made)", n.car, NativeName(c.native));
            g.dir.Report(out);
            return;
        }
    }

    LARECOMP_APP_INFO("[cars] {} {}{}", n.car, NativeName(c.native), Describe(c));
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

// ---------------------------------------------------------------- the scene runner's applier

void ApplierBegin(const Scene& scene) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.keys.clear();
    g.next_key = 0;
    g.routes.clear();
    g.path_clock = 0.0;
    g.last_scene_t = 0.0;
    g.have_scene_t = false;
    g.reads.clear();
    for (const Event& e : scene.events) {
        if (e.track != Track::kCar) continue;
        CarCommand cmd;
        std::string why;
        switch (ParseCarCommand(e.words, &cmd, &why)) {
            case CarParse::kOk:
                g.keys.push_back({e.t, e.line, cmd});
                break;
            case CarParse::kOtherVerb:
                LARECOMP_APP_WARN("[cars] line {}: 'car {} {}' is not applied: the cars package knows spawn, delete, profile, path and stop", e.line,
                                  e.words.empty() ? std::string() : e.words[0], e.words.size() > 1 ? e.words[1] : std::string());
                break;
            case CarParse::kError:
                LARECOMP_APP_ERROR("[cars] line {}: {}", e.line, why);
                break;
        }
    }
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
    } else if (t > g.last_scene_t) {
        const WorldState w = EvaluateWorld(scene, g.last_scene_t);
        const double scale = w.timescale_set ? std::clamp(w.timescale, 0.0, 10.0) : 1.0;
        g.path_clock += (t - g.last_scene_t) * scale;
    }
    g.last_scene_t = t;
    g.have_scene_t = true;
    while (g.next_key < g.keys.size() && g.keys[g.next_key].t <= t + 1e-9) {
        const CarKey& k = g.keys[g.next_key++];
        const Route* route = nullptr;
        if (k.cmd.verb == CarVerb::kPath) {
            const auto it = g.routes.find(k.cmd.route);
            if (it != g.routes.end()) route = it->second.route.get();
        }
        const Director::Applied a = g.dir.Apply(k.cmd, g.path_clock, route);
        if (a.ok) LARECOMP_APP_INFO("[cars] t={:.2f} (line {}) {}", t, k.line, a.message);
        else LARECOMP_APP_WARN("[cars] t={:.2f} (line {}) {}", t, k.line, a.message);
    }
}

void ApplierEnd() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    const bool had_cars = !g.keys.empty();
    g.keys.clear();
    g.next_key = 0;
    g.dir.EndScene();
    FlushEvents();
    const bool active = g.dir.Active();
    g_active.store(active, std::memory_order_relaxed);  // the cars it spawned are deleted over the next updates
    if (!active && had_cars) LogNothingLeft();           // ... unless the scene deleted them itself: nothing is left to wait for
}

const TrackApplier kApplier = {"cars", TrackBit(Track::kCar), &ApplierBegin, &ApplierApply, &ApplierEnd};

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

    if (world && g.frame % kWorldCheckEvery == 0) {
        for (const Director::Reader& r : g.dir.InWorld()) {
            if (InManager(base, mgr, r.racer)) continue;
            LARECOMP_APP_WARN("[cars] {}: racer {} is not in the opponent manager's list any more (the world was rebuilt?): the car is dropped", r.id, Hex(r.racer));
            g.dir.MarkLost(r.racer);
        }
    }

    const bool calls_ok = world && g.ready_ticks >= kSettleTicks;
    if (calls_ok && !g.was_ready) LARECOMP_APP_INFO("[cars] the world is ready for native calls (a car on the road, not paused, the opponent manager at {})", Hex(mgr));
    g.was_ready = calls_ok;
    const Director::Next next = g.dir.Poll(now, player, calls_ok);
    if (next.call.native != Native::kNone) Execute(base, next, mgr);
    if (calls_ok) TickPaths(base, mgr);

    FlushEvents();
    LogPoses(now);
    if (!g.dir.Active()) {
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

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "cars.h"

namespace mc::cinematic {
void TickCars() {}
bool GetCarPose(std::string_view, CarPose*) { return false; }
bool CarMatrixById(const char*, float[16]) { return false; }
size_t SceneCarCount() { return 0; }
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
