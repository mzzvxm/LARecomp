#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's world commands (package C, DIR-ATLAS; DIRECTOR-DESIGN.md 4.2, feature-parity/DIRECTOR-ATLAS-NOTES.md): `atlas.dump` and the `query.*` commands.
//
//   atlas.dump        reads the city's lamps, the traffic rail network, the garages and the car table out of guest memory and writes the atlas files
//                     (lights.json, rails.json, garages.json, cars.json, meta.json) into <root>\atlas\<hash>\; ports\scripts\atlas.py reads them
//   query.lights      the lamps near a point            query.road      the lanes under a point (the street's name, the way it runs)
//   query.route       a route along the roads           query.ground    the ground height (the physics probe, valid where the world is streamed)
//   query.pick        what lies under a point of the picture   query.visible   where a subject is in the picture, and whether something is in the way
//   query.garages     the three garages                 query.streaming City_DoneStreaming         query.spots   the scene's spots
//
// The live world is decoded once from guest memory (atlas_core.h: the layouts) and kept; `atlas.dump {refresh:true}` reads it again. All handlers run on the
// game thread (director.h). The guest addresses are those the notes derive from the recompiled code:
//
//   *0x8287E2F4            mcMultiLightMgr; its +4 is the run-time light set, whose +96 is the list {u32 array, u16 count, u16 capacity} of 96-byte lights
//                          (sub_822F87E8 builds it from lights.#lm) [CHECK C4: the live list is lights.xlm's list]
//   *0x8286D8E4            mcAmbientMgr; the traffic mcRailnetAmbient is at +0x641C0 (409,920), its +8 the mcRailnetData (the resource's root object)
//                          [CHECK C1: the live arrays are the file's]
//   0x828CD0A8             the car table (cars.cpp)
//   0x828DA464 / 0x82574418  the physics level and its segment probe (map_warp.cpp: the teleport snap's)
//

#if __has_include("director.h")

#include <rex/kernel/title_runtime.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include <chrono>

#include "../scene_runtime.h"
#include "atlas_answers.h"
#include "atlas_core.h"
#include "director.h"
#include "larecomp_guest.h"
#include "larecomp_log.h"

namespace mc::cinematic::atlas {

namespace {

namespace dir = mc::cinematic::director;
using dir::ErrorCode;
using dir::Json;

constexpr uint32_t kMultiLightMgrPtr = 0x8287E2F4;
constexpr uint32_t kLightSetAt = 4;
constexpr uint32_t kLightListAt = 96;
constexpr uint32_t kAmbientMgrPtr = 0x8286D8E4;
constexpr uint32_t kRailnetAt = 409920;  // 0x641C0
constexpr uint32_t kRailnetDataAt = 8;
constexpr uint32_t kCarTable = 0x828CD0A8;
constexpr uint32_t kLevelPtr = 0x828DA464;
constexpr uint32_t kProbeFn = 0x82574418;
constexpr uint32_t kGuestMallocFn = 0x82130528;

// ---------------------------------------------------------------------------------------------------------------- guest memory

uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

// A view of the game's memory that cannot fault: each read is checked to lie in committed, readable pages (Windows: the last good region is remembered,
// so a decode of a few MB asks the OS a few thousand times, not a million; Linux: process_vm_readv on this process answers EFAULT for a bad page).
class GameView final : public GuestView {
  public:
    bool Read(uint32_t addr, void* dst, uint32_t bytes) const override {
        uint8_t* base = Membase();
        if (!base || addr < 0x10000 || uint64_t(addr) + bytes > 0x100000000ull) return false;
#if defined(_WIN32)
        const uint64_t end = uint64_t(addr) + bytes;
        if (addr < lo_ || end > hi_) {
            if (!Region(base, addr, bytes)) return false;
        }
        std::memcpy(dst, base + addr, bytes);
        return true;
#else
        iovec local{dst, bytes};
        iovec remote{base + addr, bytes};
        return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
#endif
    }

  private:
#if defined(_WIN32)
    bool Region(uint8_t* base, uint32_t addr, uint32_t bytes) const {
        constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        uint64_t at = addr;
        const uint64_t end = uint64_t(addr) + bytes;
        uint64_t first = 0, last = 0;
        while (at < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + at, &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kRead) == 0) return false;
            const uint64_t region_start = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
            const uint64_t region_end = region_start + mbi.RegionSize;
            if (region_end <= at) return false;
            if (at == addr) first = region_start;
            last = region_end;
            at = region_end;
        }
        lo_ = first;
        hi_ = last;
        return true;
    }
    mutable uint64_t lo_ = 1, hi_ = 0;  // the last region found readable
#endif
};

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = REX_RUNTIME();
    return rt ? rt->FindFunction(addr).value_or(nullptr) : nullptr;
}

void WriteBE32(uint32_t ea, uint32_t v) {
    uint8_t* base = Membase();
    if (!base || !ea) return;
    base[ea] = uint8_t(v >> 24);
    base[ea + 1] = uint8_t(v >> 16);
    base[ea + 2] = uint8_t(v >> 8);
    base[ea + 3] = uint8_t(v);
}

void WriteF32(uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    WriteBE32(ea, v);
}

// ---------------------------------------------------------------------------------------------------------------- the physics probe

struct Hit {
    Vec3 pos;
    Vec3 normal;
    uint32_t inst = 0;
};

uint32_t g_scratch = 0;  // guest: segment 32 + result 96

bool EnsureScratch() {
    if (g_scratch) return true;
    PPCFunc* fn = GuestFn(kGuestMallocFn);
    const uint32_t raw = fn ? larecomp::guest::Call<uint32_t>(fn, 160u) : 0;
    if (!raw) return false;
    g_scratch = (raw + 15) & ~15u;
    return true;
}

// The first thing a segment from `a` to `b` meets in the physics level (the segment probe the teleport snap runs, map_warp.cpp). 0: nothing hit; -1: the
// probe cannot run (no level yet, no guest thread); 1: a hit.
int ProbeSegment(Vec3 a, Vec3 b, Hit* out) {
    GameView view;
    PPCFunc* fn = GuestFn(kProbeFn);
    const uint32_t level = view.U32(kLevelPtr);
    uint8_t* base = Membase();
    if (!fn || !level || !base || !EnsureScratch()) return -1;
    const uint32_t seg = g_scratch;
    const uint32_t res = g_scratch + 32;
    const float s[8] = {float(a.x), float(a.y), float(a.z), 0.0f, float(b.x), float(b.y), float(b.z), 0.0f};
    for (int i = 0; i < 8; ++i) WriteF32(seg + 4 * i, s[i]);
    std::memset(base + res, 0, 96);
    base[res + 76] = 0xFF;  // component: none
    base[res + 77] = 0xFF;
    const uint32_t hit = larecomp::guest::Call<uint32_t>(fn, level, seg, res, 0u, 0x800u, 0xFFFFFFFFu, 4u, 1u, 0u);
    if (!(hit & 0xFF)) return 0;
    out->inst = view.U32(res);
    out->pos = {view.F32(res + 16), view.F32(res + 20), view.F32(res + 24)};
    out->normal = {view.F32(res + 32), view.F32(res + 36), view.F32(res + 40)};
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------- the live world

struct Live {
    std::unique_ptr<World> world;
    std::unique_ptr<Index> index;
    std::chrono::steady_clock::time_point read_at;
};
Live g_live;

// Reads the world from guest memory (or again, with refresh). False with `error` when no city is loaded (a menu, a loading screen: the ambient manager is
// built at the boot, with its rail network, but the lights and the player's car are not there until a city is: run 1 of 11-20 dumped a menu's world, kept it,
// and every later query.lights answered from it) or when neither the lights nor the roads are there. A world that was read incompletely (a part is named in
// its `error`) is not kept for good: the next call reads it again, at most every 1.5 s, so a query that comes after the city has finished loading sees all of it.
bool EnsureWorld(bool refresh, std::string* error) {
    GameStateSample gs;
    if (!mc::cinematic::SampleGameState(&gs) || !gs.world_ready) {
        *error = "no city is loaded (a menu or a loading screen): load a save and leave the menus";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_live.world && !refresh) {
        const bool partial = !g_live.world->error.empty();
        if (!partial || now - g_live.read_at < std::chrono::milliseconds(1500)) return true;
    }
    auto w = std::make_unique<World>();
    w->source = "live";
    GameView view;
    std::string problems;
    auto note = [&problems](const char* part, const std::string& e) {
        if (!e.empty()) problems += std::string(problems.empty() ? "" : "; ") + part + ": " + e;
    };
    if (!ReadGarages(view, &w->garages)) w->garages = StaticGarages();

    std::string e;
    const uint32_t light_mgr = view.U32(kMultiLightMgrPtr);
    const uint32_t light_set = light_mgr ? view.U32(light_mgr + kLightSetAt) : 0;
    if (light_set) {
        if (!DecodeLights(view, light_set + kLightListAt, &w->lights, &e)) note("lights", e);
    } else {
        note("lights", "the light manager is not made yet");
    }
    e.clear();
    const uint32_t amb = view.U32(kAmbientMgrPtr);
    const uint32_t root = amb ? view.U32(amb + kRailnetAt + kRailnetDataAt) : 0;
    if (root) {
        if (!DecodeRailnet(view, root, w.get(), &e)) {
            note("rails", e);
            w->nodes.clear();
            w->rails.clear();
        }
    } else {
        note("rails", "the ambient manager's rail network is not loaded");
    }
    e.clear();
    if (!DecodeCars(view, kCarTable, &w->cars, &e)) note("cars", e);
    w->error = problems;
    if (w->lights.empty() && w->rails.empty()) {
        *error = problems.empty() ? "no city is loaded" : problems;
        return false;
    }
    g_live.index.reset();
    g_live.world = std::move(w);
    g_live.index = std::make_unique<Index>(*g_live.world);
    g_live.read_at = now;
    LARECOMP_APP_INFO("[atlas] live world read: {} lights, {} rails, {} nodes, {} cars{}", g_live.world->lights.size(), g_live.world->rails.size(),
                      g_live.world->nodes.size(), g_live.world->cars.size(), problems.empty() ? std::string() : " (" + problems + ")");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- helpers

bool SafeRelative(const std::string& p) {
    if (p.empty() || p.find("..") != std::string::npos || p[0] == '/' || p[0] == '\\') return false;
    if (p.size() >= 2 && p[1] == ':') return false;
    return true;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

void BadArgs(const dir::Responder& rs, const std::string& msg) { rs.Fail(ErrorCode::kBadArgs, msg); }

dir::ArgSpec Arg(const char* name, dir::ArgType type, bool required, const char* doc) { return dir::ArgSpec{name, type, required, doc}; }

// ---------------------------------------------------------------------------------------------------------------- handlers

void HandleDump(const dir::Request& rq, const dir::Responder& rs) {
    const std::string sub = rq.args.StringOr("dir", "atlas");
    if (!SafeRelative(sub)) return rs.Fail(ErrorCode::kRefused, "dir must be a relative folder name with no '..' and no drive: '" + sub + "'");
    std::string err;
    if (!EnsureWorld(rq.args.BoolOr("refresh", true), &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    const World& w = *g_live.world;
    const dir::ListenerInfo li = dir::GetListenerInfo();
    std::filesystem::path root = li.info_path.empty() ? std::filesystem::current_path() : std::filesystem::path(li.info_path).parent_path();
    const std::string hash = ContentHash(w);
    const std::filesystem::path out = root / sub / hash;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) return rs.Fail(ErrorCode::kFailed, "cannot create " + out.string() + ": " + ec.message());
    struct File {
        const char* name;
        std::string text;
    };
    std::vector<File> files;
    files.push_back({"lights.json", LightsJson(w)});
    files.push_back({"rails.json", RailsJson(w)});
    files.push_back({"garages.json", GaragesJson(w)});
    files.push_back({"cars.json", CarsJson(w)});
    files.push_back({"meta.json", MetaJson(w)});
    Json written = Json::Array();
    for (const File& f : files) {
        if (!WriteFile(out / f.name, f.text)) return rs.Fail(ErrorCode::kFailed, "cannot write " + (out / f.name).string());
        written.Push(f.name);
    }
    WriteFile(root / sub / "latest.txt", hash);
    Json counts = Json::Object();
    counts.Set("lights", static_cast<int64_t>(w.lights.size()));
    counts.Set("rails", static_cast<int64_t>(w.rails.size()));
    counts.Set("nodes", static_cast<int64_t>(w.nodes.size()));
    counts.Set("garages", static_cast<int64_t>(w.garages.size()));
    counts.Set("cars", static_cast<int64_t>(w.cars.size()));
    Json res = Json::Object();
    res.Set("dir", out.string());
    res.Set("hash", hash);
    res.Set("files", std::move(written));
    res.Set("counts", std::move(counts));
    if (!w.error.empty()) res.Set("partial", w.error);
    LARECOMP_APP_INFO("[atlas] dump written to {}", out.string());
    rs.Done(std::move(res));
}

void HandleLights(const dir::Request& rq, const dir::Responder& rs) {
    std::string err;
    if (!EnsureWorld(false, &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    if (g_live.world->lights.empty()) return rs.Fail(ErrorCode::kNoWorld, "the city's lights are not loaded: " + g_live.world->error);
    Json res;
    const auto snap = dir::GetSnapshot();
    if (!AnswerLights(*g_live.index, rq.args, snap.get(), &res, &err)) return BadArgs(rs, err);
    rs.Done(std::move(res));
}

void HandleRoad(const dir::Request& rq, const dir::Responder& rs) {
    std::string err;
    if (!EnsureWorld(false, &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    if (g_live.world->rails.empty()) return rs.Fail(ErrorCode::kNoWorld, "the rail network is not loaded: " + g_live.world->error);
    Json res;
    const auto snap = dir::GetSnapshot();
    if (!AnswerRoad(*g_live.index, rq.args, snap.get(), &res, &err)) return BadArgs(rs, err);
    rs.Done(std::move(res));
}

void HandleRoute(const dir::Request& rq, const dir::Responder& rs) {
    std::string err;
    if (!EnsureWorld(false, &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    if (g_live.world->rails.empty()) return rs.Fail(ErrorCode::kNoWorld, "the rail network is not loaded: " + g_live.world->error);
    Json res;
    const auto snap = dir::GetSnapshot();
    if (!AnswerRoute(*g_live.index, rq.args, snap.get(), &res, &err)) return rs.Fail(ErrorCode::kFailed, err);
    rs.Done(std::move(res));
}

void HandleGarages(const dir::Request&, const dir::Responder& rs) {
    World w;
    std::string err;
    if (g_live.world) {
        w.garages = g_live.world->garages;
    } else {
        GameView view;
        if (!ReadGarages(view, &w.garages)) w.garages = StaticGarages();
    }
    Json res;
    AnswerGarages(w, &res);
    rs.Done(std::move(res));
}

void HandleStreaming(const dir::Request&, const dir::Responder& rs) {
    bool known = false;
    const bool done = dir::StreamingDone(&known);
    Json res = Json::Object();
    res.Set("done", done);
    res.Set("known", known);
    rs.Done(std::move(res));
}

void HandleSpots(const dir::Request& rq, const dir::Responder& rs) {
    const auto snap = dir::GetSnapshot();
    Vec3 origin;
    bool has_near = false, has_y = false;
    std::string err;
    if (rq.args.Has("near")) {
        if (!ResolvePosition(rq.args.Get("near"), snap.get(), &origin, &has_y, &err)) return BadArgs(rs, err);
        has_near = true;
    }
    const double r = rq.args.NumberOr("r", 1e9);
    Json list = Json::Array();
    if (snap) {
        for (const dir::SnapSpot& s : snap->spots) {
            const double d = std::sqrt((s.pos[0] - origin.x) * (s.pos[0] - origin.x) + (s.pos[2] - origin.z) * (s.pos[2] - origin.z));
            if (has_near && d > r) continue;
            Json o = Json::Object();
            o.Set("name", s.name);
            o.Set("pos", Json::Vec3(s.pos[0], s.pos[1], s.pos[2]));
            o.Set("heading", s.heading);
            o.Set("text", s.text);
            if (has_near) o.Set("dist", std::round(d * 100.0) / 100.0);
            list.Push(std::move(o));
        }
    }
    Json res = Json::Object();
    res.Set("spots", std::move(list));
    rs.Done(std::move(res));
}

void NotStreamed(const dir::Responder& rs, const char* detail) {
    Json d = Json::Object();
    d.Set("why", "not streamed");
    rs.Fail(ErrorCode::kNotReady, std::string("not streamed: ") + detail, std::move(d));
}

void HandleGround(const dir::Request& rq, const dir::Responder& rs) {
    std::vector<std::pair<double, double>> pts;
    const bool batch = rq.args.Has("points");
    if (batch) {
        for (const Json& p : rq.args.Get("points").items()) {
            if (p.is_array() && p.size() == 2 && p.at(0).is_number() && p.at(1).is_number()) pts.emplace_back(p.at(0).AsNumber(), p.at(1).AsNumber());
            else if (p.is_object() && p.Get("x").is_number() && p.Get("z").is_number()) pts.emplace_back(p.Get("x").AsNumber(), p.Get("z").AsNumber());
            else return BadArgs(rs, "points are [x, z] or {x, z}");
        }
        if (pts.empty() || pts.size() > 256) return BadArgs(rs, "points: 1 to 256 points");
    } else if (rq.args.Get("x").is_number() && rq.args.Get("z").is_number()) {
        pts.emplace_back(rq.args.Get("x").AsNumber(), rq.args.Get("z").AsNumber());
    } else {
        return BadArgs(rs, "give x and z, or points");
    }
    bool known = false;
    if (!dir::StreamingDone(&known) && known) return NotStreamed(rs, "the city is still streaming");
    std::string err;
    const bool have_atlas = EnsureWorld(false, &err) && !g_live.world->rails.empty();
    Json out = Json::Array();
    int probed = 0, unreadable = 0;
    for (const auto& p : pts) {
        // from the roads' height + 20 m when the atlas knows the place (a roof is not the ground: the probe starts below the tallest things near it),
        // else from a fixed 300 m; `from_y` overrides
        double top = rq.args.NumberOr("from_y", 300.0);
        double road_y = 0.0, road_d = 0.0;
        if (!rq.args.Has("from_y") && have_atlas && GroundFromRails(*g_live.index, p.first, p.second, &road_y, &road_d)) top = road_y + 20.0;
        Hit hit;
        const int r = ProbeSegment({p.first, top, p.second}, {p.first, -100.0, p.second}, &hit);
        Json o = Json::Object();
        o.Set("x", p.first);
        o.Set("z", p.second);
        if (r == 1) {
            o.Set("y", std::round(hit.pos.y * 100.0) / 100.0);
            o.Set("normal", Json::Vec3(std::round(hit.normal.x * 1000.0) / 1000.0, std::round(hit.normal.y * 1000.0) / 1000.0, std::round(hit.normal.z * 1000.0) / 1000.0));
            o.Set("source", "probe");
            ++probed;
        } else {
            o.Set("y", Json());
            o.Set("why", "not streamed");
            if (r < 0) ++unreadable;
        }
        out.Push(std::move(o));
    }
    if (!batch) {
        if (probed == 0) return NotStreamed(rs, unreadable ? "the physics level is not up" : "nothing to stand on at that point yet");
        Json first = out.at(0);
        rs.Done(std::move(first));
        return;
    }
    Json res = Json::Object();
    res.Set("points", std::move(out));
    rs.Done(std::move(res));
}

void HandlePick(const dir::Request& rq, const dir::Responder& rs) {
    const double u = rq.args.NumberOr("u", -1.0), v = rq.args.NumberOr("v", -1.0);
    if (!(u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0)) return BadArgs(rs, "u and v are the picture's 0..1 coordinates, origin top left");
    const auto snap = dir::GetSnapshot();
    CameraPose pose;
    std::string err;
    const Json* over = rq.args.Has("camera") ? &rq.args.Get("camera") : nullptr;
    if (!ResolveCamera(over, snap.get(), &pose, &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    Vec3 o, d;
    RayThrough(pose, u, v, &o, &d);
    const double max_dist = rq.args.NumberOr("max_dist", 500.0);
    Hit hit;
    const int r = ProbeSegment(o, {o.x + d.x * max_dist, o.y + d.y * max_dist, o.z + d.z * max_dist}, &hit);
    if (r < 0) return rs.Fail(ErrorCode::kNoWorld, "the physics level is not up");
    Json res = Json::Object();
    res.Set("hit", r == 1);
    res.Set("ray", Json::Vec3(d.x, d.y, d.z));
    if (r == 1) {
        const double dist = std::sqrt((hit.pos.x - o.x) * (hit.pos.x - o.x) + (hit.pos.y - o.y) * (hit.pos.y - o.y) + (hit.pos.z - o.z) * (hit.pos.z - o.z));
        res.Set("pos", Json::Vec3(std::round(hit.pos.x * 100.0) / 100.0, std::round(hit.pos.y * 100.0) / 100.0, std::round(hit.pos.z * 100.0) / 100.0));
        res.Set("normal", Json::Vec3(std::round(hit.normal.x * 1000.0) / 1000.0, std::round(hit.normal.y * 1000.0) / 1000.0, std::round(hit.normal.z * 1000.0) / 1000.0));
        res.Set("dist_m", std::round(dist * 100.0) / 100.0);
        // a scene car within 3 m of the hit is what was picked
        if (snap) {
            double best = 3.0;
            for (const dir::SnapCar& c : snap->cars) {
                const double dc = std::sqrt((c.pos[0] - hit.pos.x) * (c.pos[0] - hit.pos.x) + (c.pos[1] - hit.pos.y) * (c.pos[1] - hit.pos.y) +
                                            (c.pos[2] - hit.pos.z) * (c.pos[2] - hit.pos.z));
                if (dc < best) {
                    best = dc;
                    res.Set("car", c.id);
                }
            }
        }
    }
    rs.Done(std::move(res));
}

void HandleVisible(const dir::Request& rq, const dir::Responder& rs) {
    if (!rq.args.Has("subject")) return BadArgs(rs, "subject is needed: a position, \"player\", \"car:<id>\" or \"spot:<name>\"");
    const auto snap = dir::GetSnapshot();
    Vec3 subject;
    bool has_y = false;
    std::string err;
    if (!ResolvePosition(rq.args.Get("subject"), snap.get(), &subject, &has_y, &err)) return BadArgs(rs, err);
    CameraPose pose;
    const Json* over = rq.args.Has("camera") ? &rq.args.Get("camera") : nullptr;
    if (!ResolveCamera(over, snap.get(), &pose, &err)) return rs.Fail(ErrorCode::kNoWorld, err);
    const bool is_car = rq.args.Get("subject").is_string() && (rq.args.Get("subject").AsString() == "player" || rq.args.Get("subject").AsString().rfind("car:", 0) == 0);
    const double radius = rq.args.NumberOr("radius", is_car ? 2.5 : 0.5);
    Json res;
    AnswerProjection(pose, subject, radius, &res);
    // Something in the way: a hit by the segment from the camera to the subject that is closer than the subject (its radius and a metre of slack off). null
    // when the probe cannot run (CHECK C7: the level holds the props and the buildings, so a hit is real; a miss is not proof)
    Json occluded;
    Hit hit;
    const double dist = res.Get("dist_m").AsNumber();
    const int r = ProbeSegment(pose.pos, subject, &hit);
    if (r >= 0) {
        const double hd = std::sqrt((hit.pos.x - pose.pos.x) * (hit.pos.x - pose.pos.x) + (hit.pos.y - pose.pos.y) * (hit.pos.y - pose.pos.y) +
                                    (hit.pos.z - pose.pos.z) * (hit.pos.z - pose.pos.z));
        occluded = Json(r == 1 && hd < dist - radius - 1.0);
    }
    res.Set("occluded", std::move(occluded));
    rs.Done(std::move(res));
}

// ---------------------------------------------------------------------------------------------------------------- registration

const dir::CommandRegistrar kDump(
    "atlas.dump", HandleDump,
    dir::Schema{"query",
                "write the atlas files (lamps, roads and lanes, garages, car table) read from the running game",
                {Arg("dir", dir::ArgType::kString, false, "folder under the run's folder, default \"atlas\": files go to <dir>/<hash>/"),
                 Arg("refresh", dir::ArgType::kBool, false, "read the game's memory again (default true)")},
                "{dir, hash, files, counts, partial?}"});
const dir::CommandRegistrar kLights(
    "query.lights", HandleLights,
    dir::Schema{"query",
                "the lamps near a point, closest first",
                {Arg("near", dir::ArgType::kAny, true, "[x,y,z] | [x,z] | {x,z} | \"player\" | \"car:id\" | \"spot:name[@r,u,f]\""),
                 Arg("r", dir::ArgType::kNumber, false, "radius in metres, default 60"), Arg("n", dir::ArgType::kInt, false, "most lights returned, default 20"),
                 Arg("min_intensity", dir::ArgType::kNumber, false, "default 0 (a street lamp is 20)"), Arg("kind", dir::ArgType::kString, false, "\"spot\" or \"omni\"")},
                "{count, in_radius, lights:[{i,pos,color,dir,intensity,cone,spread,type,on,dist,reach_m}]}"});
const dir::CommandRegistrar kRoad(
    "query.road", HandleRoad,
    dir::Schema{"query",
                "the lanes under a point: the street, its speed limit, the way it runs",
                {Arg("pos", dir::ArgType::kAny, true, "[x,y,z] | [x,z] | {x,z} | \"player\" | \"car:id\" | \"spot:name\""),
                 Arg("r", dir::ArgType::kNumber, false, "radius in metres, default 40"), Arg("n", dir::ArgType::kInt, false, "most lanes returned, default 6"),
                 Arg("heading", dir::ArgType::kNumber, false, "degrees: only lanes that run within 75 degrees of it")},
                "{on_road, nearest:[{rail,node,kind,name,speed,dir,dist_m,t,pos,heading_deg,len_m,next}]}"});
const dir::CommandRegistrar kRoute(
    "query.route", HandleRoute,
    dir::Schema{"query",
                "a drivable route along the roads between two places",
                {Arg("from", dir::ArgType::kAny, true, "a position, a spot, a car, or {rail, t}"), Arg("to", dir::ArgType::kAny, true, "the same"),
                 Arg("via", dir::ArgType::kArray, false, "places the route passes in order (at most 16)"),
                 Arg("speed", dir::ArgType::kNumber, false, "a cap in m/s (default: the roads' limits)"), Arg("heading", dir::ArgType::kNumber, false, "the way the car at `from` points")},
                "{points:[[x,z,speed,y]], rails, length_m, duration_s, start, end, names}"});
const dir::CommandRegistrar kGround(
    "query.ground", HandleGround,
    dir::Schema{"query",
                "the ground height under a point: the physics probe, valid where the world is streamed (else not_ready, why \"not streamed\")",
                {Arg("x", dir::ArgType::kNumber, false, "with z"), Arg("z", dir::ArgType::kNumber, false, "with x"),
                 Arg("points", dir::ArgType::kArray, false, "[[x,z],...] up to 256, instead of x and z"),
                 Arg("from_y", dir::ArgType::kNumber, false, "where the probe starts (default: 20 m over the nearest road, else 300)")},
                "{x, z, y, normal, source:\"probe\"} or, with points, {points:[{x,z,y|null,why?}]}"});
const dir::CommandRegistrar kPick(
    "query.pick", HandlePick,
    dir::Schema{"query",
                "what lies under a point of the picture: the first thing the ray through it meets",
                {Arg("u", dir::ArgType::kNumber, true, "0..1 across, left to right"), Arg("v", dir::ArgType::kNumber, true, "0..1 down, top to bottom"),
                 Arg("max_dist", dir::ArgType::kNumber, false, "metres, default 500"), Arg("camera", dir::ArgType::kObject, false, "{pos,yaw,pitch,roll?,fov?}: instead of the camera")},
                "{hit, ray, pos?, normal?, dist_m?, car?}"});
const dir::CommandRegistrar kGarages("query.garages", HandleGarages, dir::Schema{"query", "the three garages' trigger points", {}, "{garages:[{name,pos}]}"});
const dir::CommandRegistrar kStreaming("query.streaming", HandleStreaming, dir::Schema{"query", "whether the world's streaming is done", {}, "{done, known}"});
const dir::CommandRegistrar kVisible(
    "query.visible", HandleVisible,
    dir::Schema{"query",
                "where a subject is in the picture, how big, and whether something is in the way",
                {Arg("subject", dir::ArgType::kAny, true, "a position, \"player\", \"car:id\", \"spot:name\""),
                 Arg("camera", dir::ArgType::kObject, false, "{pos,yaw,pitch,roll?,fov?}: instead of the camera"),
                 Arg("radius", dir::ArgType::kNumber, false, "the subject's radius in metres (a car 2.5, a point 0.5)")},
                "{in_front, in_frame, screen:[u,v]|null, size_frac, dist_m, depth_m, occluded:true|false|null}"});
const dir::CommandRegistrar kSpots(
    "query.spots", HandleSpots,
    dir::Schema{"query",
                "the scene's named spots (spots.txt)",
                {Arg("near", dir::ArgType::kAny, false, "a position: spots within r of it get a dist"), Arg("r", dir::ArgType::kNumber, false, "metres, default all")},
                "{spots:[{name,pos,heading,text,dist?}]}"});

}  // namespace

}  // namespace mc::cinematic::atlas

#endif  // __has_include("director.h")

#endif  // REXGLUE_HAS_XEO3_TARGET
