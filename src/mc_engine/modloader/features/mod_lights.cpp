#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Dynamic lights a mod adds to its own part of the map: a second MCLA light
// grid, switched in while the camera is over the mod's rectangle.
//
// At night MCLA lights the city and the cars per pixel from a light grid
// (the PSMultiLight shaders): a 512x512 texture of light indices over a
// rectangle, and three textures of light data. The rectangle is hard-coded
// around LA, so ground a mod adds outside it gets no lamp light at all. Such a
// mod ships
//
//   <exe>/models/<mod>/lights.bin
//
// and this builds a complete second grid object for it with the game's own
// builder, over the mod's rectangle, right after the game has set up the
// city's. The shader globals point at that grid while the camera is inside the
// rectangle, and at the city's otherwise; the city's grid is never touched.
//
// lights.bin, little-endian: 'MLGT' u32 version(1) u32 count u32 record size
// (56) f32 grid rectangle min x, min z, max x, max z (all 0: the lights' extent
// plus 100 m); then per light f32 position xyz, colour rgb (0..1), direction
// xyz, intensity (MCLA's units: 20 is a LA street lamp, which reaches
// sqrt(100 * 20) = 44.7 m), cone angle (deg, 360 = omni), cone spread (deg),
// u32 id, u8 type (0 omni, 1 spot), u8 flags (1 = on), u16 0.
//
// RE map (default.xex):
//
//   *0x8287E2F4              mcMultiLightMgr
//     +16                    shader globals: +0 LightSortScaleBias, +16 PositionScale,
//                            +32 PositionBias, +48 PositionTexture, +52 PositionTexture2,
//                            +56 ColorT1, +60 LightIndexGrid1, +64 ColorT2,
//                            +68 LightIndexGrid2 (+0..+71 written only by sub_822F87E8)
//     +592                   the city's grid object (288 B)
//   sub_82302078(globals)    sets the shader globals from them: every frame and
//                            before every multilight draw
//   sub_822F87E8(mgr)        after lights.xlm loads: builds the city's grid over
//                            X -3467..2853, Z -2358..1213 and points the globals at it
//   sub_82302FA0(grid, 128, 256, 512)   creates a grid object's six textures
//                            (+16 PositionTexture, +24 ColorT1, +28 LightIndexGrid1,
//                            +32 ColorT2, +36 LightIndexGrid2)
//   sub_82303A58(grid, list, &sphere, &min, &max)   builds it. list = {u32 light
//                            pointers, u16 count, u16 capacity}; entry 0 is the null
//                            light 0x8288B850, which every empty cell points at
//   sub_82302038(globals, grid+28, grid+36) and sub_82302158(globals, grid+16,
//                            grid+20, grid+24, grid+32, &sphere, &min, &max) point the
//                            globals at a grid; here they write a scratch copy
//   0x8288B7D0..0x8288B82F   the builder's packing scale and bias, which the city's
//                            runtime light set keeps unpacking with: saved and
//                            restored around the mod's build
//   light (96 B)             +0 position, +16 colour, +32 direction, +48 unused,
//                            +64 intensity, +68 cone angle, +72 cone spread (0 means
//                            1), +76/+80 cone falloff (0/1 omni, sub_822FD2C8(f1 angle,
//                            f2 spread, r5 +76, r6 +80) for a spot), +88 type, +89 on
//   dword_82839704 +368      camera position (current viewport)
//

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "../../logging.h"
#include "mod_data.h"

REXCVAR_DEFINE_BOOL(mod_lights, true, "MCLA/World",
    "Dynamic lights a mod adds to its part of the map (models/<mod>/lights.bin): its lamps "
    "light the ground and the cars at night, on a light grid of its own. Built at city load.")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mod_light_intensity, 1.0, "MCLA/World",
    "Mod light intensity multiplier, applied when the mod's light grid is built (city load).")
    .debug_only();

REX_EXTERN(__imp__rex_sub_822F87E8);
REX_EXTERN(__imp__rex_sub_82302078);
REX_EXTERN(rex_sub_82130528);  // guest malloc
REX_EXTERN(rex_sub_82130588);  // guest free
REX_EXTERN(rex_sub_82302FA0);
REX_EXTERN(rex_sub_82303A58);
REX_EXTERN(rex_sub_82302038);
REX_EXTERN(rex_sub_82302158);
REX_EXTERN(rex_sub_822FD2C8);

namespace {

constexpr uint32_t kGlobalsAt = 16;
constexpr uint32_t kStateBytes = 72;  // the globals the grid decides: +0..+71
constexpr uint32_t kGridBytes = 288;
constexpr uint32_t kNullLight = 0x8288B850;
constexpr uint32_t kPackBlock = 0x8288B7D0;
constexpr uint32_t kPackBytes = 0x60;
constexpr uint32_t kViewportPtr = 0x82839704;
constexpr uint32_t kCameraPosAt = 368;
constexpr uint32_t kLightBytes = 96;
constexpr uint32_t kMaxLights = 128 * 256 - 1;  // the light textures, less the null light
constexpr uint32_t kNaN = 0x7FC00001;
constexpr float kMargin = 100.0f;
constexpr uint32_t kMagic = 0x54474C4D;  // 'MLGT'
constexpr char kFileName[] = "lights.bin";

#pragma pack(push, 1)
struct Header {
    uint32_t magic, version, count, record;
    float min_x, min_z, max_x, max_z;
};
struct Light {
    float x, y, z;
    float r, g, b;
    float dx, dy, dz;
    float intensity, cone, spread;
    uint32_t id;
    uint8_t type, flags;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 32);
static_assert(sizeof(Light) == 56);
constexpr uint8_t kOn = 1;
constexpr uint8_t kSpot = 1;

struct ModGrid {
    std::string mod;
    std::vector<Light> lights;
    float min_x = 0, min_z = 0, max_x = 0, max_z = 0;
    uint32_t object = 0;  // guest grid object; its textures live for the session
    std::array<uint8_t, kStateBytes> state{};
    bool built = false;
};

std::once_flag g_load_once;
std::mutex g_mu;  // g_grids' state/built, g_globals, g_city_state, g_active
std::vector<ModGrid> g_grids;
uint32_t g_globals = 0;
std::array<uint8_t, kStateBytes> g_city_state{};
int g_active = -1;  // grid the globals point at: -1 the city's

uint32_t ReadBE32(const uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float ReadF32(const uint8_t* base, uint32_t ea) {
    const uint32_t bits = ReadBE32(base, ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteBE32(uint8_t* base, uint32_t ea, uint32_t v) {
    base[ea + 0] = uint8_t(v >> 24);
    base[ea + 1] = uint8_t(v >> 16);
    base[ea + 2] = uint8_t(v >> 8);
    base[ea + 3] = uint8_t(v);
}

void WriteBE16(uint8_t* base, uint32_t ea, uint16_t v) {
    base[ea + 0] = uint8_t(v >> 8);
    base[ea + 1] = uint8_t(v);
}

void WriteF32(uint8_t* base, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    WriteBE32(base, ea, v);
}

void WriteVec(uint8_t* base, uint32_t ea, float x, float y, float z, float w) {
    WriteF32(base, ea + 0, x);
    WriteF32(base, ea + 4, y);
    WriteF32(base, ea + 8, z);
    WriteF32(base, ea + 12, w);
}

// A guest function on a fresh context below the caller's frame (what
// GuestToHostFunction does, plus the two float arguments).
uint32_t Call(PPCFunc* fn, PPCContext& ctx, uint8_t* base, std::initializer_list<uint32_t> gpr,
              double f1 = 0.0, double f2 = 0.0) {
    PPCContext c{};
    c.r1 = ctx.r1;
    c.r1.u32 -= 0x70;
    c.r13 = ctx.r13;
    c.fpscr = ctx.fpscr;
    PPCRegister* regs[] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10};
    size_t i = 0;
    for (uint32_t v : gpr) regs[i++]->u64 = v;
    c.f1.f64 = f1;
    c.f2.f64 = f2;
    fn(c, base);
    ctx.fpscr = c.fpscr;
    return c.r3.u32;
}

void Load() {
    for (const ModDataFile& file : FindModDataFiles(kFileName)) {
        std::ifstream in(file.path, std::ios::binary);
        Header h{};
        if (!in.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != kMagic || h.version != 1 ||
            h.record != sizeof(Light) || h.count == 0 || h.count > kMaxLights) {
            MC_WARN("[mod-lights] {}: not a lights file", file.path.string());
            continue;
        }
        ModGrid grid;
        grid.mod = file.mod_name;
        grid.lights.resize(h.count);
        if (!in.read(reinterpret_cast<char*>(grid.lights.data()), std::streamsize(sizeof(Light) * h.count))) {
            MC_WARN("[mod-lights] {}: truncated", file.path.string());
            continue;
        }
        if (h.max_x > h.min_x && h.max_z > h.min_z) {
            grid.min_x = h.min_x;
            grid.min_z = h.min_z;
            grid.max_x = h.max_x;
            grid.max_z = h.max_z;
        } else {
            grid.min_x = grid.min_z = std::numeric_limits<float>::max();
            grid.max_x = grid.max_z = std::numeric_limits<float>::lowest();
            for (const Light& l : grid.lights) {
                grid.min_x = std::min(grid.min_x, l.x - kMargin);
                grid.min_z = std::min(grid.min_z, l.z - kMargin);
                grid.max_x = std::max(grid.max_x, l.x + kMargin);
                grid.max_z = std::max(grid.max_z, l.z + kMargin);
            }
        }
        g_grids.push_back(std::move(grid));
    }
}

void WriteLight(PPCContext& ctx, uint8_t* base, uint32_t at, const Light& l, float mult) {
    std::memset(base + at, 0, kLightBytes);
    WriteVec(base, at + 0, l.x, l.y, l.z, 0.0f);
    WriteVec(base, at + 16, l.r, l.g, l.b, 0.0f);
    WriteVec(base, at + 32, l.dx, l.dy, l.dz, 0.0f);
    for (uint32_t k : {12u, 28u, 44u, 48u, 52u, 56u, 60u}) WriteBE32(base, at + k, kNaN);
    WriteF32(base, at + 64, l.intensity * mult);
    WriteF32(base, at + 68, l.type == kSpot ? l.cone : 360.0f);
    WriteF32(base, at + 72, l.spread != 0.0f ? l.spread : 1.0f);
    WriteF32(base, at + 76, 0.0f);
    WriteF32(base, at + 80, 1.0f);
    base[at + 88] = l.type == kSpot ? kSpot : 0;
    base[at + 89] = (l.flags & kOn) ? 1 : 0;
    if (l.type == kSpot)
        Call(rex_sub_822FD2C8, ctx, base, {0, 0, at + 76, at + 80}, ReadF32(base, at + 68), ReadF32(base, at + 72));
}

// The mod's grid object (created once, in `object`) with its lights built into
// it; `state` gets the globals as they read pointing at it.
bool Build(PPCContext& ctx, uint8_t* base, const ModGrid& grid, uint32_t& object,
           std::array<uint8_t, kStateBytes>& state) {
    if (!object) {
        const uint32_t raw = Call(rex_sub_82130528, ctx, base, {kGridBytes + 16});
        if (!raw) return false;
        const uint32_t obj = (raw + 15) & ~15u;
        std::memset(base + obj, 0, kGridBytes);
        Call(rex_sub_82302FA0, ctx, base, {obj, 128, 256, 512});
        if (!ReadBE32(base, obj + 16) || !ReadBE32(base, obj + 24) || !ReadBE32(base, obj + 28) ||
            !ReadBE32(base, obj + 32) || !ReadBE32(base, obj + 36)) {
            MC_WARN("[mod-lights] {}: the grid's textures were not created", grid.mod);
            return false;
        }
        object = obj;
    }
    const uint32_t obj = object;
    const uint32_t n = uint32_t(grid.lights.size());

    // sphere, min, max (16 each), scratch globals (80), list (16), pointers, lights.
    const uint32_t ptr_bytes = (4 * (n + 1) + 15) & ~15u;
    const uint32_t raw = Call(rex_sub_82130528, ctx, base, {16 + 48 + 80 + 16 + ptr_bytes + kLightBytes * n});
    if (!raw) {
        MC_WARN("[mod-lights] {}: out of guest memory for {} lights", grid.mod, n);
        return false;
    }
    const uint32_t sphere = (raw + 15) & ~15u;
    const uint32_t vmin = sphere + 16, vmax = sphere + 32, scratch = sphere + 48, list = sphere + 128;
    const uint32_t ptrs = list + 16, lights = ptrs + ptr_bytes;

    const float mult = float(REXCVAR_GET(mod_light_intensity));
    WriteBE32(base, ptrs, kNullLight);
    for (uint32_t i = 0; i < n; ++i) {
        WriteLight(ctx, base, lights + kLightBytes * i, grid.lights[i], mult);
        WriteBE32(base, ptrs + 4 * (i + 1), lights + kLightBytes * i);
    }
    WriteBE32(base, list, ptrs);
    WriteBE16(base, list + 4, uint16_t(n + 1));
    WriteBE16(base, list + 6, uint16_t(n + 1));

    // The city's grid spans y -31..79; the lights' height does not matter to it.
    const float y0 = -100.0f, y1 = 500.0f;
    WriteVec(base, vmin, grid.min_x, y0, grid.min_z, 0.0f);
    WriteVec(base, vmax, grid.max_x, y1, grid.max_z, 0.0f);
    const float ex = grid.max_x - grid.min_x, ey = y1 - y0, ez = grid.max_z - grid.min_z;
    WriteVec(base, sphere, (grid.min_x + grid.max_x) * 0.5f, (y0 + y1) * 0.5f, (grid.min_z + grid.max_z) * 0.5f,
             0.5f * std::sqrt(ex * ex + ey * ey + ez * ez));

    Call(rex_sub_82303A58, ctx, base, {obj, list, sphere, vmin, vmax});
    std::memset(base + scratch, 0, 80);
    Call(rex_sub_82302038, ctx, base, {scratch, ReadBE32(base, obj + 28), ReadBE32(base, obj + 36)});
    Call(rex_sub_82302158, ctx, base,
         {scratch, ReadBE32(base, obj + 16), ReadBE32(base, obj + 20), ReadBE32(base, obj + 24),
          ReadBE32(base, obj + 32), sphere, vmin, vmax});
    std::memcpy(state.data(), base + scratch, kStateBytes);
    Call(rex_sub_82130588, ctx, base, {raw});

    MC_INFO("[mod-lights] {}: {} lights, grid X {:.0f}..{:.0f} Z {:.0f}..{:.0f} ({:.1f} x {:.1f} m cells)",
            grid.mod, n, grid.min_x, grid.max_x, grid.min_z, grid.max_z, ex / 512.0f, ez / 512.0f);
    return true;
}

// The city's lights are set up (load, or a reload): its grid state as the
// game left it, then the mods' grids built beside it.
void OnCityLights(PPCContext& ctx, uint8_t* base, uint32_t mgr) {
    std::call_once(g_load_once, Load);
    const uint32_t globals = mgr + kGlobalsAt;
    {
        std::lock_guard lock(g_mu);
        g_globals = globals;
        std::memcpy(g_city_state.data(), base + globals, kStateBytes);
        g_active = -1;
    }
    if (g_grids.empty() || !REXCVAR_GET(mod_lights)) return;

    std::array<uint8_t, kPackBytes> pack;
    std::memcpy(pack.data(), base + kPackBlock, kPackBytes);
    for (ModGrid& grid : g_grids) {
        uint32_t object = grid.object;  // written by this thread only
        std::array<uint8_t, kStateBytes> state{};
        const bool built = Build(ctx, base, grid, object, state);
        std::lock_guard lock(g_mu);
        grid.object = object;
        grid.state = state;
        grid.built = built;
    }
    std::memcpy(base + kPackBlock, pack.data(), kPackBytes);
}

// Before the globals are set: point them at the grid the camera is over.
void SelectGrid(uint8_t* base, uint32_t globals) {
    std::lock_guard lock(g_mu);
    if (globals != g_globals || g_grids.empty()) return;
    int want = -1;
    const uint32_t vp = ReadBE32(base, kViewportPtr);
    if (vp && REXCVAR_GET(mod_lights)) {
        const float cx = ReadF32(base, vp + kCameraPosAt + 0);
        const float cz = ReadF32(base, vp + kCameraPosAt + 8);
        for (size_t i = 0; i < g_grids.size(); ++i) {
            const ModGrid& g = g_grids[i];
            if (g.built && cx >= g.min_x && cx <= g.max_x && cz >= g.min_z && cz <= g.max_z) {
                want = int(i);
                break;
            }
        }
    }
    if (want == g_active) return;
    std::memcpy(base + globals, want < 0 ? g_city_state.data() : g_grids[size_t(want)].state.data(), kStateBytes);
    if (want < 0)
        MC_INFO("[mod-lights] camera back over the city's light grid");
    else
        MC_INFO("[mod-lights] camera over {}'s light grid", g_grids[size_t(want)].mod);
    g_active = want;
}

}  // namespace

extern "C" REX_FUNC(rex_sub_822F87E8) {
    const uint32_t mgr = ctx.r3.u32;
    __imp__rex_sub_822F87E8(ctx, base);
    const uint64_t ret = ctx.r3.u64;
    OnCityLights(ctx, base, mgr);
    ctx.r3.u64 = ret;
}

extern "C" REX_FUNC(rex_sub_82302078) {
    SelectGrid(base, ctx.r3.u32);
    __imp__rex_sub_82302078(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
