#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Warp the player's car to a named point, to any point, and back.
//
// A mod that adds ground the road network does not reach (a map placed apart
// from the city) names its landing points next to its files:
//
//   <exe>/models/<mod>/warps.txt      name x y z [heading in degrees, 0 = +Z]
//
// F9 (bind_map_warp) takes the car to the first of them -- or to warp_spot when
// that is set -- and back to where it was; the console command reaches any.
//
// RE map (default.xex):
//
//   dword_82874374            player manager
//   sub_821E80F8(mgr, i)      player slot i is live
//   sub_822A3998(mgr, i)      that player's racer
//   sub_822A9930(racer, mtx, vel, angvel, snap, up, down)
//                             the racer teleport under the script native
//                             Racer_Teleport (sub_822AF0F8 -> sub_822AB838, which
//                             builds mtx from a heading and passes snap 0, 1, 3).
//                             With snap set it first probes the physics level
//                             (sub_82574418) from pos + up*Y down to pos - down*Y
//                             and sits the car on what it hits, aligned to the
//                             hit normal. Then racer+288 = mtx, +432 = vel,
//                             +448 = angvel, +464 = 1, and on the entity: vfunc
//                             24 (set matrix), collider velocities, reset.
//   racer+48                  the car entity (0 = none spawned)
//   *(entity+28)+16           its live matrix, 4 rows: right, up, back, position
//   dword_828CCEA0            pause state: +4 paused, +10 run-one-frame
//   dword_827E0DC8            mcCity: +20 sector array (304 B each), +26 count,
//                             sector +208 AABB min, +224 max
//
// The warp runs from the per-frame housekeeping hook (game thread); the console
// command and the key only file a request, since both arrive on the UI thread.
//

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "logging.h"
#include "map_warp.h"
#include "modloader/features/mod_data.h"

REXCVAR_DEFINE_STRING(warp_spot, "", "MCLA/World",
    "Where F9 drops the car: \"x y z heading\" (heading in degrees, 0 = facing +Z). "
    "Empty = the first point a mod names in models/<mod>/warps.txt. The car is "
    "dropped onto whatever collision is below the point.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kPlayerMgrPtr    = 0x82874374;
constexpr uint32_t kPlayerValidFn   = 0x821E80F8;
constexpr uint32_t kPlayerRacerFn   = 0x822A3998;
constexpr uint32_t kRacerTeleportFn = 0x822A9930;
constexpr uint32_t kGuestMallocFn   = 0x82130528;
constexpr uint32_t kPauseStatePtr   = 0x828CCEA0;
constexpr uint32_t kCityPtr         = 0x827E0DC8;
constexpr uint32_t kRacerEntityAt   = 48;
constexpr uint32_t kEntityMatrixAt  = 28;
constexpr uint32_t kSectorStride    = 304;
constexpr uint32_t kSectorMinAt     = 208;
constexpr uint32_t kSectorMaxAt     = 224;
constexpr uint32_t kPlayerSweep     = 4;
// The physics level and its segment probe, the one the teleport snap runs:
// sub_82574418(level, seg[2], result, exclude, 0x800, -1, 4, 1, exclude).
// result +0 the phInst hit, +16 the hit position, +32 the normal.
constexpr uint32_t kLevelPtr        = 0x828DA464;
constexpr uint32_t kProbeFn         = 0x82574418;

// A place to put the car: ground point and the direction to face on the
// ground plane.
struct Spot {
    float x, y, z;
    float fx, fz;
};

struct NamedSpot {
    std::string name;
    std::string mod;
    Spot spot;
};

// The snap probe runs from pos + kProbeUp to pos - kProbeDown.
constexpr double kProbeUp = 10.0;
constexpr double kProbeDown = 30.0;

enum class Kind { kNone, kNamed, kBack, kToggle, kPoint, kReport };

struct Request {
    Kind kind = Kind::kNone;
    Spot spot{};
    std::string name;
};

std::mutex g_mu;
Request g_request;

// Where the car was before its last warp out, so "back" returns there.
bool g_have_back = false;
bool g_warped_out = false;
float g_back[16] = {};

bool g_spots_loaded = false;
std::vector<NamedSpot> g_spots;

uint32_t g_scratch = 0;          // guest: matrix (64) + velocity (16) + angvel (16)
std::atomic<bool> g_bind_registered{false};
int g_watch_frames = -1;         // frames since the last warp, for the fall check
float g_watch_y = 0.0f;

void File(const Request& r) {
    std::lock_guard lock(g_mu);
    g_request = r;
}

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadBE32(uint32_t ea) {
    uint8_t* base = Membase();
    if (!base || !ea) return 0;
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float ReadF32(uint32_t ea) {
    const uint32_t bits = ReadBE32(ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteF32(uint32_t ea, float f) {
    uint8_t* base = Membase();
    if (!base || !ea) return;
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    base[ea + 0] = uint8_t(v >> 24);
    base[ea + 1] = uint8_t(v >> 16);
    base[ea + 2] = uint8_t(v >> 8);
    base[ea + 3] = uint8_t(v);
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

bool GameIsPaused() {
    const uint32_t p = ReadBE32(kPauseStatePtr);
    uint8_t* base = Membase();
    if (!p || !base) return false;
    return base[p + 4] != 0 && base[p + 10] == 0;
}

uint32_t PlayerRacer() {
    const uint32_t mgr = ReadBE32(kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(racer_fn, mgr, i)) return r;
    }
    return 0;
}

// The car's live matrix (16 floats, rows right/up/back/position), or false.
bool CarMatrix(uint32_t racer, float out[16]) {
    const uint32_t ent = ReadBE32(racer + kRacerEntityAt);
    const uint32_t mtx = ent ? ReadBE32(ent + kEntityMatrixAt) : 0;
    if (!mtx) return false;
    for (int i = 0; i < 16; ++i) out[i] = ReadF32(mtx + 16 + 4 * i);
    return true;
}

// Some city sector covers (x, z): there is a map to land on.
bool CityCovers(float x, float z) {
    const uint32_t city = ReadBE32(kCityPtr);
    if (!city) return false;
    const uint32_t sectors = ReadBE32(city + 20);
    const uint32_t count = ReadBE32(city + 24) & 0xFFFF;
    for (uint32_t i = 0; sectors && i < count; ++i) {
        const uint32_t s = sectors + i * kSectorStride;
        if (x >= ReadF32(s + kSectorMinAt) && x <= ReadF32(s + kSectorMaxAt) &&
            z >= ReadF32(s + kSectorMinAt + 8) && z <= ReadF32(s + kSectorMaxAt + 8))
            return true;
    }
    return false;
}

bool EnsureScratch() {
    if (g_scratch) return true;
    PPCFunc* malloc_fn = GuestFn(kGuestMallocFn);
    // matrix 64 + velocity 16 + angular velocity 16, probe segment 32, result 96
    const uint32_t raw = malloc_fn ? rex::ppc::GuestToHostFunction<uint32_t>(malloc_fn, 240u) : 0;
    if (!raw) return false;
    g_scratch = (raw + 15) & ~15u;
    return true;
}

// Straight down through the physics level from (x, y, z), `len` metres -- the
// probe the teleport snap itself runs. True with the ground height when some
// collision answers.
bool ProbeGround(float x, float y, float z, float len, float* ground) {
    PPCFunc* fn = GuestFn(kProbeFn);
    const uint32_t level = ReadBE32(kLevelPtr);
    uint8_t* base = Membase();
    if (!fn || !level || !base || !EnsureScratch()) return false;
    const uint32_t seg = g_scratch + 96;
    const uint32_t res = g_scratch + 128;
    const float s[8] = {x, y, z, 0.0f, x, y - len, z, 0.0f};
    for (int i = 0; i < 8; ++i) WriteF32(seg + 4 * i, s[i]);
    std::memset(base + res, 0, 96);
    base[res + 76] = 0xFF;  // component: none
    base[res + 77] = 0xFF;
    const uint32_t hit = rex::ppc::GuestToHostFunction<uint32_t>(fn, level, seg, res, 0u, 0x800u,
                                                                  0xFFFFFFFFu, 4u, 1u, 0u);
    if (!(hit & 0xFF)) return false;
    *ground = ReadF32(res + 20);
    return true;
}

bool Teleport(uint32_t racer, const float m[16], bool snap) {
    PPCFunc* fn = GuestFn(kRacerTeleportFn);
    if (!fn || !EnsureScratch()) return false;
    for (int i = 0; i < 16; ++i) WriteF32(g_scratch + 4 * i, m[i]);
    for (int i = 0; i < 8; ++i) WriteF32(g_scratch + 64 + 4 * i, 0.0f);
    rex::ppc::GuestToHostFunction<uint32_t>(fn, racer, g_scratch, g_scratch + 64, g_scratch + 80,
                                            snap ? 1u : 0u, kProbeUp, kProbeDown);
    return true;
}

// Rows right, up, back, position for a car standing on the ground plane.
void SpotMatrix(const Spot& s, float m[16]) {
    float fx = s.fx, fz = s.fz;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-4f) {
        fx = 0.0f;
        fz = 1.0f;
    } else {
        fx /= len;
        fz /= len;
    }
    const float rows[16] = {-fz, 0.0f, fx, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,
                            -fx, 0.0f, -fz, 0.0f, s.x, s.y + 1.0f, s.z, 0.0f};
    std::memcpy(m, rows, sizeof(rows));
}

Spot HeadingSpot(float x, float y, float z, float heading_deg) {
    const float h = heading_deg * 0.017453292f;
    return {x, y, z, std::sin(h), std::cos(h)};
}

bool ParseFloats(std::string_view s, float* out, int max, int* n) {
    std::string buf(s);
    const char* p = buf.c_str();
    *n = 0;
    while (*p && *n < max) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) return false;
        out[(*n)++] = v;
        p = end;
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
    }
    return *p == '\0';
}

void LoadSpots() {
    if (g_spots_loaded) return;
    g_spots_loaded = true;
    for (const ModDataFile& file : FindModDataFiles("warps.txt")) {
        std::ifstream in(file.path);
        std::string line;
        while (std::getline(in, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream ls(line);
            std::string name;
            if (!(ls >> name)) continue;
            std::string rest;
            std::getline(ls, rest);
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
            float v[4];
            int n = 0;
            if (!ParseFloats(rest, v, 4, &n) || n < 3) {
                MC_WARN("[warp] {}: '{}' needs name x y z [heading]", file.path.string(), line);
                continue;
            }
            g_spots.push_back({name, file.mod_name, HeadingSpot(v[0], v[1], v[2], n > 3 ? v[3] : 0.0f)});
        }
    }
    if (!g_spots.empty())
        MC_INFO("[warp] {} warp point(s) from mods; F9 goes to '{}' ({})", g_spots.size(), g_spots[0].name,
                g_spots[0].mod);
}

// F9's destination: warp_spot, else the first mod warp point.
bool DefaultSpot(Spot* out, std::string* name) {
    const std::string text = REXCVAR_GET(warp_spot);
    float v[4];
    int n = 0;
    if (!text.empty() && ParseFloats(text, v, 4, &n) && n >= 3) {
        *out = HeadingSpot(v[0], v[1], v[2], n > 3 ? v[3] : 0.0f);
        *name = "warp_spot";
        return true;
    }
    LoadSpots();
    if (g_spots.empty()) return false;
    *out = g_spots[0].spot;
    *name = g_spots[0].name;
    return true;
}

bool FindSpot(const std::string& name, Spot* out) {
    LoadSpots();
    for (const NamedSpot& s : g_spots) {
        if (s.name == name) {
            *out = s.spot;
            return true;
        }
    }
    return false;
}

void RegisterBindOnce() {
    if (g_bind_registered.exchange(true)) return;
    rex::ui::RegisterBind("bind_map_warp", "F9",
                          "Warp the car to a mod's warp point, and back to where it was",
                          [] { File({Kind::kToggle, {}, {}}); });
}

void Warp(const Request& r) {
    if (GameIsPaused()) return;  // keep it filed until the pause menu closes
    {
        std::lock_guard lock(g_mu);
        g_request.kind = Kind::kNone;
    }

    const uint32_t racer = PlayerRacer();
    float cur[16];
    if (!racer || !CarMatrix(racer, cur)) {
        MC_WARN("[warp] no player car right now (menu, garage or loading) -- drive first");
        return;
    }
    if (r.kind == Kind::kReport) {
        LoadSpots();
        MC_INFO("[warp] car at {:.1f} {:.1f} {:.1f}, facing {:.2f} {:.2f}", cur[12], cur[13], cur[14], -cur[8],
                -cur[10]);
        for (const NamedSpot& s : g_spots)
            MC_INFO("[warp]   {} ({}): {:.1f} {:.1f} {:.1f}", s.name, s.mod, s.spot.x, s.spot.y, s.spot.z);
        return;
    }

    Kind kind = r.kind;
    Spot spot = r.spot;
    std::string where = r.name;
    if (kind == Kind::kToggle) {
        if (g_warped_out) {
            kind = Kind::kBack;
        } else if (DefaultSpot(&spot, &where)) {
            kind = Kind::kPoint;
        } else {
            MC_WARN("[warp] no warp point: no mod names one in models/<mod>/warps.txt and warp_spot is empty");
            return;
        }
    } else if (kind == Kind::kNamed) {
        if (!FindSpot(r.name, &spot)) {
            MC_WARN("[warp] no warp point named '{}' (warp: list them)", r.name);
            return;
        }
        kind = Kind::kPoint;
    }

    float m[16];
    if (kind == Kind::kBack) {
        if (!g_have_back) {
            MC_WARN("[warp] nowhere to go back to");
            return;
        }
        std::memcpy(m, g_back, sizeof(m));
        m[13] += 1.0f;
        where = "back";
    } else {
        if (!CityCovers(spot.x, spot.z)) {
            MC_WARN("[warp] no city sector covers {:.1f} {:.1f}: the mod that adds that ground is not "
                    "loaded, so there is nothing to land on",
                    spot.x, spot.z);
            return;
        }
        SpotMatrix(spot, m);
        if (!g_warped_out) {
            std::memcpy(g_back, cur, sizeof(g_back));
            g_have_back = true;
        }
    }

    float ground = 0.0f;
    if (!ProbeGround(m[12], m[13] + float(kProbeUp), m[14], float(kProbeUp + kProbeDown), &ground))
        MC_WARN("[warp] no collision under {:.1f} {:.1f} -- the car will fall", m[12], m[14]);
    if (!Teleport(racer, m, true)) {
        MC_WARN("[warp] teleport function unavailable");
        return;
    }
    g_warped_out = kind != Kind::kBack;
    float now[16];
    CarMatrix(racer, now);
    MC_INFO("[warp] {} : {:.1f} {:.1f} {:.1f} -> asked {:.1f} {:.1f} {:.1f}, landed {:.1f} {:.1f} {:.1f}",
            where.empty() ? "point" : where, cur[12], cur[13], cur[14], m[12], m[13], m[14], now[12], now[13],
            now[14]);
    g_watch_frames = 0;
    g_watch_y = now[13];
}

// A car that lands on real collision settles within a metre or so; one that
// has nothing under it keeps falling. Logged a few times after each warp.
void WatchAfterWarp() {
    if (g_watch_frames < 0) return;
    ++g_watch_frames;
    if (g_watch_frames != 30 && g_watch_frames != 120 && g_watch_frames != 300) return;
    const uint32_t racer = PlayerRacer();
    float m[16];
    if (!racer || !CarMatrix(racer, m)) return;
    const float drop = g_watch_y - m[13];
    MC_INFO("[warp] +{} frames: car at {:.1f} {:.1f} {:.1f} ({})", g_watch_frames, m[12], m[13],
            m[14], drop > 20.0f ? "FALLING -- no collision under it" : "on the ground");
    if (g_watch_frames == 300) g_watch_frames = -1;
}

void ConsoleWarp(std::string_view args) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    while (!args.empty() && args.back() == ' ') args.remove_suffix(1);
    if (args.empty()) {
        MC_INFO("[warp] usage: warp <point> | warp back | warp <x> <y> <z> [heading deg, 0 = +Z] "
                "-- key: bind_map_warp (F9) toggles the first point <-> back");
        File({Kind::kReport, {}, {}});
        return;
    }
    if (args == "back") {
        File({Kind::kBack, {}, {}});
        return;
    }
    float v[4];
    int n = 0;
    if (ParseFloats(args, v, 4, &n) && n >= 3) {
        File({Kind::kPoint, HeadingSpot(v[0], v[1], v[2], n > 3 ? v[3] : 0.0f), {}});
        return;
    }
    File({Kind::kNamed, {}, std::string(args)});
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(warp, ConsoleWarp, "MCLA/World",
                            "Warp the car: 'warp <point>' (a mod's warps.txt), 'warp back' (to "
                            "where it was), 'warp x y z [heading]', or 'warp' to list the points. "
                            "The car is dropped onto whatever collision is below the point.");

void TickMapWarp() {
    RegisterBindOnce();
    WatchAfterWarp();
    Request r;
    {
        std::lock_guard lock(g_mu);
        r = g_request;
    }
    if (r.kind != Kind::kNone) Warp(r);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void TickMapWarp() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
