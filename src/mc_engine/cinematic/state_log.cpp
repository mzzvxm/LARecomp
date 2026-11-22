#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The per-update state log, game side (trailer toolkit, work package M3a; the format, the file's place and the queue are in
// state_log_core.h, the way to ask for it in state_log.h). It is the E1 / E1b experiments' logger (test/cin-e1b, cin_fixedstep.cpp)
// made a product tool: no fixed-step or place-the-car test code, no heap scan (the experiment walked 512 MB for the race cars; the
// scene's cars come from M4's registry through RegisterCarSource), and a bounded hand-over to the writer thread.
//
// Cvar (live, debug-only):
//   mcla_state_log   a .csv file, or a folder that gets state.csv; empty: off (a scene's `statelog on` can still ask).
//
// What a row reads, all on the game thread at Patch_DeltaTimePre (every address is the experiment's, from the generated code and the
// notes of the repo; CHECK where no run has shown it in this build):
//   the engine timer 0x827D7500: +0x08 scaled step, +0x10 mode timer, +0x14 total, +0x18 game time, +0x2C update counter, +0x54 time
//     scale, +0x58 unscaled step
//   the generator 0x82860890 (sub_822C8618, multiplier 0x5CDCFAA7), the hour and the weather index (time_of_day.h)
//   the player's racer (player manager 0x82874374, the same two getters as scene_runtime.cpp): racer+48 -> entity, entity+28 ->
//     matrix block, 16 floats from +16 (right, up, back, position), entity+8 -> physics object, its speed at +224 (m/s: E1b's
//     placed car read 2.4e-4 at rest)
//   the camera block the game publishes after its camera tick: camera owner 0x8286D804, 16 floats from +4272 (camera_trace.cpp).
//     CHECK: with a scene camera (M1b) it may still hold the game's own camera, not the scene's
//   pause 0x828CCEA0 (+4 paused, +10 run one frame), free roam live (0x82836A28 == 1, 0x8288D000 == 0, 0x8286CF54 == 0)
//
// Written without a build or a run.
//

#include "state_log.h"

#include "settings/larecomp_settings.h"
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

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

#include "mc_engine/gameplay/time_of_day.h"
#include "larecomp_log.h"
#include "scene_runtime.h"
#include "state_log_core.h"
#include "larecomp_guest.h"

namespace mc::cinematic::statelog {

namespace {

// ---------------------------------------------------------------- guest addresses
constexpr uint32_t kClock = 0x827D7500;             // the engine timer object (the only instance)
constexpr uint32_t kClockDt = kClock + 0x08;        // the step the game takes, after the time scale (float)
constexpr uint32_t kClockModeTimer = kClock + 0x10;  // seconds since the mode loaded (float)
constexpr uint32_t kClockTotal = kClock + 0x14;     // total time (float)
constexpr uint32_t kClockGame = kClock + 0x18;      // game time (float)
constexpr uint32_t kClockFrame = kClock + 0x2C;     // the engine's own update counter (u32)
constexpr uint32_t kClockScale = kClock + 0x54;     // time scale (float)
constexpr uint32_t kClockDtUnscaled = kClock + 0x58;  // the step before the time scale (float)
constexpr uint32_t kRngState = 0x82860890;          // the software generator behind RAND_*
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;     // +4 paused, +10 run one frame
constexpr uint32_t kLeftTitle = 0x82836A28;
constexpr uint32_t kGameState = 0x8288D000;
constexpr uint32_t kCityPending = 0x8286CF54;
constexpr uint32_t kPlayerMgrPtr = 0x82874374;
constexpr uint32_t kPlayerValidFn = 0x821E80F8;     // (playerMgr, index) -> bool
constexpr uint32_t kPlayerRacerFn = 0x822A3998;     // (playerMgr, index) -> racer
constexpr uint32_t kPlayerSweep = 4;
constexpr uint32_t kRacerEntityAt = 48;             // racer -> entity
constexpr uint32_t kEntityMatrixAt = 28;            // entity -> matrix block; the live matrix is at +16 of it
constexpr uint32_t kEntityPhysAt = 8;               // entity -> the vehicle's physics object
constexpr uint32_t kPhysSpeedAt = 224;              // its speed (Racer_GetSpeed, sub_822AE468)
constexpr uint32_t kVhsmGlobal = 0x8286D804;        // camera publish block owner
constexpr uint32_t kPublishedCamera = 4272;         // + right, up, forward, position rows (camera_trace.cpp)

// ---------------------------------------------------------------- guest memory, fault-proof (a stale pointer costs a row, not the run)
uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

// No C++ objects with destructors in here: a function that carries __try cannot unwind them.
bool SafeCopy(uint32_t ea, void* dst, uint32_t bytes) {
    uint8_t* base = Membase();
    if (!base || ea < 0x10000u || uint64_t(ea) + bytes > 0x100000000ull) return false;
#if defined(_WIN32)
    __try {
        std::memcpy(dst, base + ea, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    // No structured exceptions: process_vm_readv on this very process answers an unmapped or PROT_NONE page (guest memory
    // that is reserved and not committed) with EFAULT, where memcpy would fault the game.
    iovec local{dst, bytes};
    iovec remote{base + ea, bytes};
    const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n == static_cast<ssize_t>(bytes)) return true;
    if (n < 0 && (errno == ENOSYS || errno == EPERM)) {  // the call is not allowed here: read as before
        std::memcpy(dst, base + ea, bytes);
        return true;
    }
    return false;
#endif
}

uint32_t Swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

bool ReadU32(uint32_t ea, uint32_t* v) {
    uint32_t raw = 0;
    if (!SafeCopy(ea, &raw, 4)) return false;
    *v = Swap32(raw);
    return true;
}

uint32_t U32Or0(uint32_t ea) {
    uint32_t v = 0;
    return ReadU32(ea, &v) ? v : 0;
}

float F32Or0(uint32_t ea) {
    uint32_t v = 0;
    if (!ReadU32(ea, &v)) return 0.0f;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool ReadFloats(uint32_t ea, float* out, int n) {
    uint32_t raw[64];
    if (n < 1 || n > 64 || !SafeCopy(ea, raw, uint32_t(n) * 4)) return false;
    for (int i = 0; i < n; ++i) {
        const uint32_t v = Swap32(raw[i]);
        std::memcpy(out + i, &v, 4);
    }
    return true;
}

bool ReadByte(uint32_t ea, uint8_t* b) {
    return SafeCopy(ea, b, 1);
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = REX_RUNTIME();
    if (!rt) return nullptr;
    return rt->FindFunction(addr).value_or(nullptr);
}

// The local player's racer (the same two getters scene_runtime.cpp and M4's cars.cpp call), or 0.
uint32_t PlayerRacer() {
    const uint32_t mgr = U32Or0(kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(larecomp::guest::Call<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = larecomp::guest::Call<uint32_t>(racer_fn, mgr, i)) return r;
    }
    return 0;
}

// ---------------------------------------------------------------- the log's state

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

double FixedStepFps() {
    // The fixed step is capture mode's (M2a: mcla_fixed_step_fps); this file does not depend on it being in the build.
    if (!larecomp::cvar::GetFlagInfo("mcla_fixed_step_fps")) return 0.0;
    return std::atof(larecomp::cvar::GetFlagByName("mcla_fixed_step_fps").c_str());
}

struct State {
    std::unique_ptr<LogWriter> writer;
    std::string open_key;      // what the open log was opened for: "cvar:<value>" or "scene:<serial>"
    std::string path;          // its file
    uint64_t tick = 0;         // updates since it opened
    uint64_t skipped = 0;      // ... of which had no player car
    Chain chain;
    uint32_t lost_pending = 0;  // rows the queue refused since the last row it took
    uint64_t first_row_tick = 0;
    bool logged_first_drop = false;
    std::chrono::steady_clock::time_point t0;
    std::string mark;           // a mark that came due since the last row

    // what the scene asks for
    bool scene_wants = false;
    uint32_t scene_serial = 0;
    std::string scene_name;
    std::string scene_dir;
    std::string take_dir;
    const CarSource* cars = nullptr;
};
State g;
std::mutex g_path_mutex;  // CurrentLogPath() may be asked from another thread

void CloseLog(const char* why) {
    if (!g.writer || !g.writer->IsOpen()) return;
    const uint64_t written = g.writer->written();
    const uint64_t dropped = g.writer->dropped();
    g.writer->Close(g.tick);
    LARECOMP_APP_INFO("[statelog] closed ({}): {} ({} updates, {} rows written, {} dropped, {} without a player car)", why, g.path, g.tick,
                      written, dropped, g.skipped);
    g.open_key.clear();
    {
        std::lock_guard<std::mutex> lock(g_path_mutex);
        g.path.clear();
    }
}

void OpenLog(const std::string& key, const std::string& cvar_value) {
    const bool explicit_file = !cvar_value.empty() && CsvPathFor(cvar_value) == cvar_value;
    std::string path = ResolveLogPath(cvar_value, g.scene_dir, g.take_dir, g.scene_name, Stamp());
    if (!explicit_file) {
        path = FirstFreePath(path, [](const std::string& p) {
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::path(std::u8string(p.begin(), p.end())), ec);
        });
    }
    if (!g.writer) g.writer = std::make_unique<LogWriter>();
    const std::string head = PreambleLine(g.scene_name, FixedStepFps(), Stamp()) + ColumnLine() + "\n";
    if (!g.writer->Open(path, head)) {
        LARECOMP_APP_ERROR("[statelog] cannot open {}", path);
        g.open_key = "failed:" + key;  // do not try again every update; a change of what is asked tries again
        return;
    }
    g.open_key = key;
    {
        std::lock_guard<std::mutex> lock(g_path_mutex);
        g.path = path;
    }
    g.tick = 0;
    g.skipped = 0;
    g.chain = Chain{};
    g.lost_pending = 0;
    g.logged_first_drop = false;
    g.mark.clear();
    g.t0 = std::chrono::steady_clock::now();
    LARECOMP_APP_INFO("[statelog] open: {} ({} columns{})", path, ColumnNames().size(),
                      g.scene_wants && cvar_value.empty() ? ", asked for by the scene" : ", asked for by mcla_state_log");
}

// Opens, keeps, switches or closes the log so that it is what is asked for now. Cheap when nothing changed.
void Reconcile() {
    const std::string& cvar = larecomp::settings::mcla_state_log.Get();
    std::string want_key;
    if (!cvar.empty()) want_key = "cvar:" + cvar;
    else if (g.scene_wants) want_key = "scene:" + std::to_string(g.scene_serial);
    if (want_key.empty()) {
        if (!g.open_key.empty()) CloseLog("not asked for any more");
        return;
    }
    const bool is_open = g.writer && g.writer->IsOpen();
    if (g.open_key == want_key && is_open) return;
    if (g.open_key == "failed:" + want_key) return;
    if (is_open) CloseLog(g.open_key.rfind("cvar:", 0) == 0 && cvar.empty() ? "mcla_state_log was cleared" : "another log was asked for");
    OpenLog(want_key, cvar);
}

// ---------------------------------------------------------------- one row

void Gather(Sample& m) {
    m.scene_state = static_cast<int>(CurrentSceneState());
    m.scene_t = (CurrentSceneState() == SceneState::kRunning || CurrentSceneState() == SceneState::kFinished) ? SceneTime() : -1.0;
    m.dt = F32Or0(kClockDt);
    m.dt_unscaled = F32Or0(kClockDtUnscaled);
    m.time_scale = F32Or0(kClockScale);
    m.clock_total = F32Or0(kClockTotal);
    m.clock_game = F32Or0(kClockGame);
    m.mode_timer = F32Or0(kClockModeTimer);
    m.frame_ctr = U32Or0(kClockFrame);
    m.rng = U32Or0(kRngState);
    m.hour = TimeOfDay_GetHour();
    m.weather = Weather_GetIndex();
    {
        const uint32_t p = U32Or0(kPauseStatePtr);
        uint8_t a = 0, b = 0;
        if (p) {
            ReadByte(p + 4, &a);
            ReadByte(p + 10, &b);
        }
        m.paused = (a != 0 && b == 0) ? 1 : 0;
    }
    m.live = (U32Or0(kLeftTitle) == 1 && U32Or0(kGameState) == 0 && U32Or0(kCityPending) == 0) ? 1 : 0;
    // the player: the racer's entity's world matrix
    if (const uint32_t racer = PlayerRacer()) {
        const uint32_t entity = U32Or0(racer + kRacerEntityAt);
        const uint32_t mtx = entity ? U32Or0(entity + kEntityMatrixAt) : 0;
        if (mtx && ReadFloats(mtx + 16, m.p_m, kMatrixWords)) {
            m.p_ok = true;
            const uint32_t phys = U32Or0(entity + kEntityPhysAt);
            m.p_speed = phys ? F32Or0(phys + kPhysSpeedAt) : 0.0f;
        }
    }
    // the camera block the game publishes after its camera tick
    const uint32_t vhsm = U32Or0(kVhsmGlobal);
    if (vhsm && ReadFloats(vhsm + kPublishedCamera, m.cam, kMatrixWords)) m.cam_ok = true;
    // the scene's cars
    if (g.cars && g.cars->count && g.cars->get) {
        const size_t n = g.cars->count();
        m.n_cars = static_cast<int>(n);
        for (size_t i = 0; i < n && i < static_cast<size_t>(kMaxCars); ++i) {
            CarSample& c = m.car[i];
            char id[kIdChars + 1] = {};
            float mat[kMatrixWords] = {};
            c.ok = g.cars->get(i, id, sizeof id, mat);
            std::memcpy(c.id, id, sizeof c.id);
            c.id[kIdChars] = '\0';
            if (c.ok) std::memcpy(c.m, mat, sizeof c.m);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- the public side

void Tick() {
    // Cheap exit: nothing asked for and nothing open.
    if (larecomp::settings::mcla_state_log.Get().empty() && !g.scene_wants && g.open_key.empty()) return;
    Reconcile();
    if (!g.writer || !g.writer->IsOpen()) return;

    Sample m;
    m.tick = g.tick++;
    m.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.t0).count();
    m.mark = std::move(g.mark);
    g.mark.clear();
    Gather(m);
    if (!m.p_ok) {  // a menu, a loading screen, the garage: no car on the road, no row
        ++g.skipped;
        if (!m.mark.empty()) g.mark = std::move(m.mark);  // the mark waits for the first row that exists
        return;
    }
    m.lost = g.lost_pending;
    if (g.writer->Push(RowText(m, &g.chain))) {
        g.lost_pending = 0;
    } else {
        ++g.lost_pending;
        if (!g.logged_first_drop) {
            g.logged_first_drop = true;
            LARECOMP_APP_WARN("[statelog] the writer is behind: a row was dropped (tick {}); the next row's `lost` column says how many", m.tick);
        }
    }
}

void SceneBegin(const Scene& scene) {
    g.scene_wants = scene.statelog;
    g.scene_name = scene.name;
    g.scene_dir = scene.statelog_dir;
    ++g.scene_serial;  // a scene that starts again is a new take: a new file
    Reconcile();
}

void SceneEnd() {
    g.scene_wants = false;
    Reconcile();
}

void NoteMark(const std::string& name) {
    if (!Active()) return;
    g.mark = g.mark.empty() ? name : g.mark + ";" + name;
}

void SetTakeFolder(const std::string& folder) {
    g.take_dir = folder;
}

bool RegisterCarSource(const CarSource* source) {
    if (!source || !source->count || !source->get) return false;
    g.cars = source;
    return true;
}

bool Active() {
    return g.writer && g.writer->IsOpen();
}

std::string CurrentLogPath() {
void CloseForExit() {
    CloseLog("the game is ending");
}

    std::lock_guard<std::mutex> lock(g_path_mutex);
    return g.path;
}

}  // namespace mc::cinematic::statelog

#else  // REXGLUE_HAS_XEO3_TARGET

#include "state_log.h"

namespace mc::cinematic::statelog {
void Tick() {}
void SceneBegin(const Scene&) {}
void SceneEnd() {}
void NoteMark(const std::string&) {}
void SetTakeFolder(const std::string&) {}
bool RegisterCarSource(const CarSource*) { return false; }
bool Active() { return false; }
std::string CurrentLogPath() { return {}; }
void CloseForExit() {}
}  // namespace mc::cinematic::statelog

#endif  // REXGLUE_HAS_XEO3_TARGET
