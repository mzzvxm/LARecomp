#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Breakable props that a mod baked into its own city sectors.
//
// MCLA breaks props through the fragment system (sc_props.xck); geometry a mod
// adds to the city sectors is static and its collision sits in city.xbnd, so
// a pole there stops the car dead. A mod that wants such a prop to break off
// when hit hard enough -- like the game it came from did -- ships
//
//   <exe>/models/<mod>/breakables.bin
//
// listing, per prop: where it stands, how fast a hit must be to break it and
// how much speed the car keeps, the sector and name hashes of its upright
// models and of two knocked-over copies the sector carries culled, and the
// city.xbnd cell vertices of its collision box (that cell quantized with
// `headroom` metres to spare above it). This, once per frame:
//
//   * a slow bump changes nothing: the box stops the car;
//   * when the player's car reaches a prop faster than its break speed, the
//     box's vertices are lifted `headroom` metres up, out of every BVH node's
//     box, so nothing collides with it any more; the upright models are culled
//     and the knocked-over copy on the side the car was going gets its real
//     bounds back; the car keeps `keep` of its speed (sub_8255A1B0, the setter
//     the teleport uses, which also matches the wheel spin);
//   * mod_glows skips glows linked to a broken prop (ModBreakableDown);
//   * a broken prop stands up again once the car is kRestoreDistance away.
//
// A sector that streams out and back in comes back with the upright models;
// the patch is re-applied when its data pointer changes.
//
// breakables.bin, little-endian:
//   'MBRK' u32 version(1) u32 count f32 headroom
//   per prop: u32 id, f32 base[3], f32 fall[3], f32 radius, f32 height,
//             f32 break speed (m/s), f32 speed kept (0..1), char sector[16],
//             u8 n_up, u8 n_fallen, u8 n_cells, u8 0, u32 up_hash[n_up],
//             2 x n_fallen x {u32 hash, f32 centre[3], f32 radius, f32 min[3],
//             f32 max[3]} (fallen towards +fall, then -fall),
//             n_cells x {u16 cell, u16 n, u16 vertex[n]} padded to 4 bytes
//
// RE map (default.xex):
//
//   dword_828742E0              city bound resource (sub_8229E680 loads
//                               city.#bnd into it): +40 root, root+48 phInst,
//                               +4 archetype, +12 phBoundGrid; grid +0xa0 cell
//                               array (+0xa4 count), cell (phBoundBVH) +0x90
//                               quantum, +0xa0 centre, +0xb0 vertices int16 x3
//   dword_827E0DC8              mcCity: +20 sector array (304 B), +26 count;
//                               sector +240 name, +248 stream entry (+0 the
//                               resident mcCitySectorData, vtable 0x8203CBB8),
//                               +252 state (3 resident)
//   mcCitySectorData            +28 blob (n spheres of 16 B, then n boxes of
//                               32 B), +32 mcCityModel array (80 B: +16 sphere,
//                               +32 min, +48 max, +76 name hash), +36 count
//   dword_82874374 +8           the player's racer (game thread)
//   racer +48                   car entity: +28 -> +16 matrix rows (position
//                               at +64), +8 the vehicle
//   vehicle +128                its collider: +192 mass, +272 velocity,
//                               +288 angular velocity, +304/+320 momenta
//   sub_8255A1B0(vehicle, &vel, &angvel)
//

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../logging.h"
#include "mod_breakables.h"
#include "mod_data.h"

REXCVAR_DEFINE_BOOL(mod_breakables, true, "MCLA/World",
    "Props a mod baked into the city sectors break off when the car hits them fast "
    "enough (models/<mod>/breakables.bin); a slow bump still stops the car.")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mod_breakable_speed_scale, 1.0, "MCLA/World",
    "Multiplier on the speed a mod's breakable prop needs to break.")
    .debug_only();

namespace {

constexpr uint32_t kCityBoundPtr = 0x828742E0;
constexpr uint32_t kCityPtr = 0x827E0DC8;
constexpr uint32_t kSectorStride = 304;
constexpr uint32_t kSectorNameAt = 240;
constexpr uint32_t kSectorEntryAt = 248;
constexpr uint32_t kSectorStateAt = 252;
constexpr uint32_t kSectorDataVft = 0x8203CBB8;
constexpr uint32_t kPlayerMgrPtr = 0x82874374;
constexpr uint32_t kSetVelocityFn = 0x8255A1B0;
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr float kCarReach = 2.8f;          // m from the car's centre to its front corners
constexpr float kLookAhead = 0.04f;        // s of travel added to the reach
constexpr float kMinSpeed = 4.0f;          // m/s: nothing breaks below this
constexpr float kRestoreDistance = 350.0f; // m: a broken prop this far from the car stands up again
constexpr float kHiddenY = -100000.0f;     // culled models live here
constexpr float kGridCell = 16.0f;
constexpr uint32_t kMagic = 0x4B52424D;    // 'MBRK'
constexpr char kFileName[] = "breakables.bin";

struct Fallen {
    uint32_t hash;
    float c[3], r, mn[3], mx[3];
};

struct Cell {
    uint16_t cell;
    std::vector<uint16_t> verts;
    int16_t lifted = 0;  // quantized units added, to put it back
};

struct Prop {
    int mod;
    float headroom;
    uint32_t id;
    float base[3], fall[3];
    float radius, height, brk, keep;
    std::string sector;
    std::vector<uint32_t> up;
    std::vector<Fallen> fallen[2];
    std::vector<Cell> cells;
    // runtime
    int sector_index = -1;
    bool broken = false;
    int side = 0;
    uint32_t applied_data = 0;
    std::vector<std::array<float, 12>> up_saved;  // sphere, min, max as the sector had them
};

bool g_loaded = false;
std::vector<Prop> g_props;
std::unordered_map<int64_t, std::vector<uint32_t>> g_grid;
std::vector<uint32_t> g_broken;  // indices into g_props
std::shared_mutex g_down_mu;
std::unordered_set<uint64_t> g_down;  // (mod << 32) | id, read by the glow feed
uint32_t g_scratch = 0;               // guest: velocity (16) + angular velocity (16)

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t R32(uint8_t* b, uint32_t ea) {
    const uint8_t* p = b + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t R16(uint8_t* b, uint32_t ea) { return uint16_t((b[ea] << 8) | b[ea + 1]); }
float RF(uint8_t* b, uint32_t ea) {
    const uint32_t v = R32(b, ea);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void W32(uint8_t* b, uint32_t ea, uint32_t v) {
    b[ea] = uint8_t(v >> 24); b[ea + 1] = uint8_t(v >> 16); b[ea + 2] = uint8_t(v >> 8); b[ea + 3] = uint8_t(v);
}
void WF(uint8_t* b, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    W32(b, ea, v);
}

int64_t GridKey(float x, float z) {
    return (int64_t(std::floor(x / kGridCell)) << 32) ^ int64_t(uint32_t(int32_t(std::floor(z / kGridCell))));
}

template <typename T>
bool Take(const std::vector<uint8_t>& buf, size_t& at, T* out, size_t n = 1) {
    if (at + sizeof(T) * n > buf.size()) return false;
    std::memcpy(out, buf.data() + at, sizeof(T) * n);
    at += sizeof(T) * n;
    return true;
}

void LoadFile(const ModDataFile& file) {
    std::ifstream in(file.path, std::ios::binary);
    const std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t at = 0;
    uint32_t hdr[3];
    float headroom = 0.0f;
    if (!Take(buf, at, hdr, 3) || hdr[0] != kMagic || hdr[1] != 1 || !Take(buf, at, &headroom)) {
        MC_WARN("[breakables] {}: not a breakables file", file.path.string());
        return;
    }
    size_t loaded = 0;
    for (uint32_t i = 0; i < hdr[2]; ++i) {
        Prop p;
        p.mod = file.mod;
        p.headroom = headroom;
        char name[16];
        uint8_t counts[4];
        if (!Take(buf, at, &p.id) || !Take(buf, at, p.base, 3) || !Take(buf, at, p.fall, 3) ||
            !Take(buf, at, &p.radius) || !Take(buf, at, &p.height) || !Take(buf, at, &p.brk) ||
            !Take(buf, at, &p.keep) || !Take(buf, at, name, 16) || !Take(buf, at, counts, 4)) {
            break;
        }
        name[15] = 0;
        p.sector = name;
        p.up.resize(counts[0]);
        bool ok = Take(buf, at, p.up.data(), p.up.size());
        for (int s = 0; ok && s < 2; ++s) {
            p.fallen[s].resize(counts[1]);
            ok = Take(buf, at, p.fallen[s].data(), p.fallen[s].size());
        }
        for (int c = 0; ok && c < counts[2]; ++c) {
            Cell cell;
            uint16_t n = 0;
            ok = Take(buf, at, &cell.cell) && Take(buf, at, &n);
            cell.verts.resize(n);
            ok = ok && Take(buf, at, cell.verts.data(), n);
            at += (4 - (4 + 2 * size_t(n)) % 4) % 4;
            p.cells.push_back(std::move(cell));
        }
        if (!ok) break;
        g_grid[GridKey(p.base[0], p.base[2])].push_back(uint32_t(g_props.size()));
        g_props.push_back(std::move(p));
        ++loaded;
    }
    MC_INFO("[breakables] {}: {} breakable props (headroom {:.0f} m)", file.mod_name, loaded, headroom);
}

void Load() {
    g_loaded = true;
    for (const ModDataFile& f : FindModDataFiles(kFileName)) LoadFile(f);
}

uint32_t GridBound(uint8_t* b) {
    const uint32_t root = R32(b, kCityBoundPtr);
    const uint32_t obj = root ? R32(b, root + 40) : 0;
    const uint32_t inst = obj ? R32(b, obj + 48) : 0;
    const uint32_t arch = inst ? R32(b, inst + 4) : 0;
    const uint32_t grid = arch ? R32(b, arch + 12) : 0;
    return grid && b[grid + 4] == 8 ? grid : 0;
}

// Lift (dir 1) or put back (dir -1) the prop's box vertices.
void MoveCollision(uint8_t* b, Prop& p, int dir) {
    const uint32_t grid = GridBound(b);
    if (!grid) return;
    const uint32_t cells = R32(b, grid + 0xa0);
    const uint16_t ncells = R16(b, grid + 0xa4);
    for (Cell& c : p.cells) {
        if (c.cell >= ncells) continue;
        const uint32_t cellp = R32(b, cells + 4u * c.cell);
        const uint32_t verts = cellp ? R32(b, cellp + 0xb0) : 0;
        if (!verts) continue;
        if (dir > 0) {
            const float qy = RF(b, cellp + 0x94);
            if (!(qy > 0.0f)) continue;
            c.lifted = int16_t(std::clamp((p.headroom - 10.0f) / qy, 0.0f, 32000.0f));
        }
        for (uint16_t v : c.verts) {
            const uint32_t ea = verts + 6u * v + 2u;
            const int32_t y = int16_t(R16(b, ea)) + dir * c.lifted;
            const int16_t y16 = int16_t(std::clamp(y, -32767, 32767));
            b[ea] = uint8_t(uint16_t(y16) >> 8);
            b[ea + 1] = uint8_t(uint16_t(y16));
        }
    }
}

int SectorIndex(uint8_t* b, const std::string& name) {
    const uint32_t city = R32(b, kCityPtr);
    if (!city) return -1;
    const uint32_t arr = R32(b, city + 20);
    const uint32_t n = R16(b, city + 26);
    for (uint32_t i = 0; arr && i < n; ++i) {
        const uint32_t s = R32(b, arr + i * kSectorStride + kSectorNameAt);
        if (s && std::strncmp(reinterpret_cast<const char*>(b + s), name.c_str(), 16) == 0) return int(i);
    }
    return -1;
}

uint32_t SectorData(uint8_t* b, int index) {
    if (index < 0) return 0;
    const uint32_t city = R32(b, kCityPtr);
    const uint32_t arr = city ? R32(b, city + 20) : 0;
    if (!arr) return 0;
    const uint32_t s = arr + uint32_t(index) * kSectorStride;
    if (R32(b, s + kSectorStateAt) != 3) return 0;
    const uint32_t entry = R32(b, s + kSectorEntryAt);
    const uint32_t data = entry ? R32(b, entry) : 0;
    return data && R32(b, data) == kSectorDataVft ? data : 0;
}

// A model's culling bounds: its own and the sector blob's copy.
void SetBounds(uint8_t* b, uint32_t data, uint32_t index, uint32_t n, const float c[3], float r,
               const float mn[3], const float mx[3]) {
    const uint32_t m = R32(b, data + 32) + 80u * index;
    const uint32_t blob = R32(b, data + 28);
    for (int k = 0; k < 3; ++k) {
        WF(b, m + 16 + 4 * k, c[k]);
        WF(b, m + 32 + 4 * k, mn[k]);
        WF(b, m + 48 + 4 * k, mx[k]);
        WF(b, blob + 16 * index + 4 * k, c[k]);
        WF(b, blob + 16 * n + 32 * index + 4 * k, mn[k]);
        WF(b, blob + 16 * n + 32 * index + 16 + 4 * k, mx[k]);
    }
    WF(b, m + 28, r);
    WF(b, blob + 16 * index + 12, r);
}

// broken: upright models culled, the fallen copy on `side` shown; else the reverse.
void ApplyModels(uint8_t* b, Prop& p, uint32_t data, bool broken) {
    const uint32_t models = R32(b, data + 32);
    const uint32_t n = R16(b, data + 36);
    const float far[3] = {0.0f, kHiddenY, 0.0f};
    if (broken && p.up_saved.size() != p.up.size()) p.up_saved.assign(p.up.size(), {});
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t m = models + 80u * i;
        const uint32_t h = R32(b, m + 76u);
        for (size_t u = 0; u < p.up.size(); ++u) {
            if (p.up[u] != h) continue;
            if (broken) {
                for (int k = 0; k < 4; ++k) {
                    p.up_saved[u][k] = RF(b, m + 16 + 4 * k);
                    p.up_saved[u][4 + k] = RF(b, m + 32 + 4 * k);
                    p.up_saved[u][8 + k] = RF(b, m + 48 + 4 * k);
                }
                SetBounds(b, data, i, n, far, 0.01f, far, far);
            } else if (u < p.up_saved.size()) {
                const auto& s = p.up_saved[u];
                SetBounds(b, data, i, n, &s[0], s[3], &s[4], &s[8]);
            }
        }
        for (int side = 0; side < 2; ++side) {
            for (const Fallen& fl : p.fallen[side]) {
                if (fl.hash != h) continue;
                if (broken && side == p.side)
                    SetBounds(b, data, i, n, fl.c, fl.r, fl.mn, fl.mx);
                else
                    SetBounds(b, data, i, n, far, 0.01f, far, far);
            }
        }
    }
}

bool EnsureScratch() {
    if (g_scratch) return true;
    auto* rt = rex::Runtime::instance();
    PPCFunc* fn = rt && rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(kGuestMallocFn) : nullptr;
    const uint32_t raw = fn ? rex::ppc::GuestToHostFunction<uint32_t>(fn, 48u) : 0;
    if (raw) g_scratch = (raw + 15) & ~15u;
    return g_scratch != 0;
}

void SlowCar(uint8_t* b, uint32_t vehicle, uint32_t col, float keep) {
    auto* rt = rex::Runtime::instance();
    PPCFunc* fn = rt && rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(kSetVelocityFn) : nullptr;
    if (!fn || !EnsureScratch()) return;
    for (int k = 0; k < 4; ++k) {
        WF(b, g_scratch + 4 * k, RF(b, col + 272 + 4 * k) * (k < 3 ? keep : 1.0f));
        WF(b, g_scratch + 16 + 4 * k, RF(b, col + 288 + 4 * k));
    }
    rex::ppc::GuestToHostFunction<uint32_t>(fn, vehicle, g_scratch, g_scratch + 16);
}

uint64_t DownKey(int mod, uint32_t id) { return (uint64_t(uint32_t(mod)) << 32) | id; }

void Break(uint8_t* b, uint32_t index, float vx, float vz, float speed, uint32_t vehicle, uint32_t col) {
    Prop& p = g_props[index];
    p.broken = true;
    g_broken.push_back(index);
    {
        std::unique_lock lock(g_down_mu);
        g_down.insert(DownKey(p.mod, p.id));
    }
    p.side = (vx * p.fall[0] + vz * p.fall[2]) >= 0.0f ? 0 : 1;
    MoveCollision(b, p, 1);
    if (p.sector_index < 0) p.sector_index = SectorIndex(b, p.sector);
    const uint32_t data = SectorData(b, p.sector_index);
    if (data) ApplyModels(b, p, data, true);
    p.applied_data = data;
    SlowCar(b, vehicle, col, p.keep);
    MC_INFO("[breakables] prop {} knocked over at {:.0f} km/h (breaks at {:.0f}), car keeps {:.0f}%", p.id,
            speed * 3.6f, p.brk * 3.6f * float(REXCVAR_GET(mod_breakable_speed_scale)), p.keep * 100.0f);
}

void Restore(uint8_t* b, Prop& p) {
    MoveCollision(b, p, -1);
    const uint32_t data = SectorData(b, p.sector_index);
    if (data && data == p.applied_data) ApplyModels(b, p, data, false);
    p.broken = false;
    p.applied_data = 0;
    std::unique_lock lock(g_down_mu);
    g_down.erase(DownKey(p.mod, p.id));
}

// Broken props: re-applied on a sector that streamed back in, stood up again
// once the car is far enough away (or the feature is switched off).
void TendBroken(uint8_t* b, float px, float pz, bool all) {
    for (size_t k = 0; k < g_broken.size();) {
        Prop& p = g_props[g_broken[k]];
        const float dx = p.base[0] - px, dz = p.base[2] - pz;
        if (all || dx * dx + dz * dz > kRestoreDistance * kRestoreDistance) {
            Restore(b, p);
            g_broken[k] = g_broken.back();
            g_broken.pop_back();
            continue;
        }
        const uint32_t data = SectorData(b, p.sector_index);
        if (data != p.applied_data) {
            if (data) ApplyModels(b, p, data, true);
            p.applied_data = data;
        }
        ++k;
    }
}

}  // namespace

bool ModBreakableDown(int mod, uint32_t id) {
    std::shared_lock lock(g_down_mu);
    return !g_down.empty() && g_down.count(DownKey(mod, id)) != 0;
}

void TickModBreakables() {
    uint8_t* b = Membase();
    if (!b) return;
    if (!REXCVAR_GET(mod_breakables)) {
        if (!g_broken.empty()) TendBroken(b, 0.0f, 0.0f, true);
        return;
    }
    if (!g_loaded) Load();
    if (g_props.empty()) return;

    const uint32_t mgr = R32(b, kPlayerMgrPtr);
    const uint32_t racer = mgr ? R32(b, mgr + 8) : 0;
    const uint32_t ent = racer ? R32(b, racer + 48) : 0;
    const uint32_t mtx = ent ? R32(b, ent + 28) : 0;
    const uint32_t vehicle = ent ? R32(b, ent + 8) : 0;
    const uint32_t col = vehicle ? R32(b, vehicle + 128) : 0;
    if (!mtx || !col) return;
    const float px = RF(b, mtx + 64), py = RF(b, mtx + 68), pz = RF(b, mtx + 72);
    if (!g_broken.empty()) TendBroken(b, px, pz, false);

    const float vx = RF(b, col + 272), vz = RF(b, col + 280);
    const float speed = std::sqrt(vx * vx + vz * vz);
    if (!(speed > kMinSpeed)) return;
    const float scale = float(REXCVAR_GET(mod_breakable_speed_scale));
    for (int gx = -1; gx <= 1; ++gx) {
        for (int gz = -1; gz <= 1; ++gz) {
            const auto it = g_grid.find(GridKey(px + gx * kGridCell, pz + gz * kGridCell));
            if (it == g_grid.end()) continue;
            for (uint32_t i : it->second) {
                Prop& p = g_props[i];
                if (p.broken) continue;
                const float dx = p.base[0] - px, dz = p.base[2] - pz;
                const float d = std::sqrt(dx * dx + dz * dz);
                // a frame or two of travel on top: at 180 km/h the bumper closes 0.8 m a frame
                if (d > kCarReach + p.radius + speed * kLookAhead) continue;
                if (py < p.base[1] - 2.0f || py > p.base[1] + p.height) continue;
                if ((vx * dx + vz * dz) < 0.2f * d * speed) continue;  // not heading into it
                if (speed < p.brk * scale) continue;                    // a bump: the box stops the car
                Break(b, i, vx, vz, speed, vehicle, col);
            }
        }
    }
}

#else  // REXGLUE_HAS_XEO3_TARGET

bool ModBreakableDown(int, uint32_t) { return false; }
void TickModBreakables() {}

#endif  // REXGLUE_HAS_XEO3_TARGET
