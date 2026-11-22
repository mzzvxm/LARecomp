// MCLA GPU renderer -- the ray tracing WORLD FRAME, the shader side (CITY-RT package FRAME; the C++ twin and the layout's whole story are src/gpu/rt_world_frame_rules.h, the public header is
// src/gpu/rt_world_frame.h).
//
// What it gives a pass that traces the world (the SSR fallback, the probe field's update, the car reflections' city hits, the lamps and the emissive parts): in EVERY frame that has a top
// level, whether or not the player's car is in it,
//   gRtWorld  (t10)  rows 0..31 are the hit frame (rt_hit_frame.h: the world's sun, ambient, the environment map, the game's sun cascades) so `RT_HIT_FRAME_ROW(i)` = `gRtWorld[i]` makes
//                    RtHitLoadFrame and the whole city hit library read the world's light; rows 32..47 are the world rows: RtWorldLoad
//   gRtLamps  (t11)  this frame's lamps near the camera, in view space, sorted into 64 m cells: the cursor below walks the lamps around a point
//
// THE INCLUDER defines, before including:
//   RT_WORLD_SPACE            the register space of the ray tracing descriptor set (the car scene's CAR_RT_SPACE), unless it defines RT_WORLD_NO_RESOURCES and declares
//                             `StructuredBuffer<uint4> gRtWorld` and `gRtLamps` itself (the unit test does, over its own buffers)
//   RT_HIT_FRAME_ROW(i)       optional: if it is not defined this file defines it as gRtWorld[i]. A pass that has its OWN frame rows (the car passes': gCarRtMaterials[i]) defines it first
//                             and keeps its meaning; RtWorldHitFrame() below always reads the world's rows
//   RT_WORLD_LOOP             optional: the attribute of the lookup's loops (default [loop]; the test defines it empty)
//
// THE LAMP LOOP (R7: city_hit_shade.hlsli's CityHitLamps does it with `#define CITY_HIT_LAMP_GRID`, the cap and the sum's cap included; this is the loop it is made of):
//     const RtWorld w = RtWorldLoad();                                   // once a pass
//     for (RtLampCursor c = RtLampBegin(w, hitViewPosition, 64.0); c.done == 0u; c = RtLampStep(w, c))
//     {
//         const CityHitLamp lamp = CityHitLampFromRows(RtLampRow(w, c.index, 0u), RtLampRow(w, c.index, 1u), RtLampRow(w, c.index, 2u));
//         ...CityHitLampAt(p, lamp, position, normal) with p.lampFactor = w.lampFactor...
//     }
// The records' colours already hold each lamp's activity over the schedule (rt_world_frame_rules.h LAMPS): pass p.lampFactor = w.lampFactor, as the library expects. A lookup with no lamps in
// the window (day, or the world's rows are not valid) yields a cursor that is done at once.
//
// src/gpu/rt_world_frame_test.cpp compiles THIS FILE as C++ (hlsl_cpp_shim.h, RT_WORLD_NO_RESOURCES) and checks every function against the twin, and every #define against the header's numbers.
// That is why the text keeps to a subset both languages read: scalar and vector arithmetic, no out parameters (a function returns a struct), the intrinsics abs sqrt floor min max clamp saturate
// dot asfloat, RT_WORLD_LOOP for [loop].
//
// The names: every one starts with RtWorld, RtLamp or RT_WORLD_.

#ifndef MCLA_RT_WORLD_FRAME_HLSLI
#define MCLA_RT_WORLD_FRAME_HLSLI

#ifndef RT_WORLD_LOOP
#define RT_WORLD_LOOP [loop]
#endif

// ---- the numbers (src/gpu/rt_world_frame_rules.h) ---------------------------------------------------------------------------------------------

#define RT_WORLD_ROW_STATUS 32u
#define RT_WORLD_ROW_LIGHT 33u
#define RT_WORLD_ROW_CAMERA 34u
#define RT_WORLD_ROW_SUN_WORLD 35u
#define RT_WORLD_ROW_GRID 36u
#define RT_WORLD_ROW_WINDOW 37u
#define RT_WORLD_ROW_COUNTS 38u
#define RT_WORLD_ROW_CALIBRATION 39u
#define RT_WORLD_ROW_GAINS 40u
#define RT_WORLD_ROW_SCHEDULE 47u
#define RT_WORLD_TOTAL_ROWS 48u
#define RT_WORLD_CLASSES 16u

#define RT_WORLD_VALID 1u
#define RT_WORLD_HOUR 2u
#define RT_WORLD_CAMERA_KNOWN 4u
#define RT_WORLD_SUN_WORLD 8u
#define RT_WORLD_LIVE_FRESH 16u
#define RT_WORLD_LAMPS 32u
#define RT_WORLD_STATS_FRAME 64u
#define RT_WORLD_CAR 128u
#define RT_WORLD_LIGHT 256u

#define RT_WORLD_EVIDENCE_ON 1u      // the game's live lamp list had lamps that shine: the street lamps are on (rt_world_frame_rules.h kEvidenceOn)
#define RT_WORLD_EVIDENCE_OFF 2u     // a fresh list with none while the table has street lamps in its reach: they are off
#define RT_WORLD_GLOW_NIGHT 0u       // the glow kinds (city_hit_format.h kGlow*; hlsl CITY_HIT_GLOW_*)
#define RT_WORLD_GLOW_LENS 1u
#define RT_WORLD_GLOW_ALWAYS 2u

#define RT_WORLD_UNKNOWN_WEATHER 0xFFFFFFFFu
#define RT_WORLD_LOOKUP_AXIS 5       // a lookup reads at most this many cells an axis
#define RT_WORLD_LOOKUP_CELLS 25u
#define RT_WORLD_LOOKUP_MAX_CELLS 2.0 // the radius is clamped to this many cell sides
#define RT_WORLD_LAMP_ROWS 3u

#ifndef RT_WORLD_NO_RESOURCES
#ifndef RT_WORLD_SPACE
#error "define RT_WORLD_SPACE (the register space of the ray tracing descriptor set) before including rt_world_frame.hlsli"
#endif
#define RT_WORLD_JOIN_(a, b) a##b
#define RT_WORLD_JOIN(a, b) RT_WORLD_JOIN_(a, b)
#define RT_WORLD_SPACE_NAME RT_WORLD_JOIN(space, RT_WORLD_SPACE)

StructuredBuffer<uint4> gRtWorld : register(t10, RT_WORLD_SPACE_NAME);
StructuredBuffer<uint4> gRtLamps : register(t11, RT_WORLD_SPACE_NAME);
#endif

#ifndef RT_HIT_FRAME_ROW
#define RT_HIT_FRAME_ROW(i) gRtWorld[i]
#endif
#include "rt_hit_frame.hlsli"

// ---- the world rows -----------------------------------------------------------------------------------------------------------------------------

struct RtWorld
{
    uint serial;        // the scene's frame (low 32 bits)
    uint flags;         // RT_WORLD_*: read RT_WORLD_VALID first
    float hour;         // 0..24, -1 unknown
    float lampFactor;   // 0 by day .. 1 at night: the library's CityHitParams::lampFactor (lamps, lamp lenses, neon, lit windows)
    float units;        // 2^exposure bias: the paint's units times this are the picture's
    float wet;          // 0 dry .. 1 rain
    uint weather;       // 0 nice, 1 cloudy, 2 stormy, 3 foggy; RT_WORLD_UNKNOWN_WEATHER
    float time;         // the scene clock, seconds
    float3 camera;      // the camera's place in the game's world
    float sunNight;     // the world sun's night factor (NOT the lamps')
    float3 toSunWorld;  // unit, the game's world space (RT_WORLD_SUN_WORLD)
    float cellM;        // the lamp grid's cell side, metres
    float lookupM;      // the radius a lamp lookup takes by default
    float originX;
    float originZ;
    int cx0;            // the window's first cell
    int cz0;
    uint nx;            // cells across and deep
    uint nz;
    uint lampCount;     // lamps uploaded
    uint lampsInWindow; // before the budget cut
    uint cells;
    uint liveCount;     // the game's live list's size this frame
    uint statsFrame;    // 1: set CITY_HIT_P_CALIBRATE
    uint windows;       // calibration windows closed
    uint worldKind;     // 0 none, 1 play, 2 pause, 3 garage, 4 map
    float street;       // row 47 (R7): the street lamps' schedule 0..1: what a lamp lens glows by
    float fill;         // the fill lights' night factor 0..1: what neon, signs and lit windows glow by
    uint liveShining;   // the game's live lamp list's lamps that shine now
    uint evidence;      // RT_WORLD_EVIDENCE_*
    float3 axisX;       // the world's axes in view space
    float3 axisY;
    float3 axisZ;
};

RtWorld RtWorldLoad()
{
    const uint4 r0 = gRtWorld[RT_WORLD_ROW_STATUS];
    const uint4 r1 = gRtWorld[RT_WORLD_ROW_LIGHT];
    const uint4 r2 = gRtWorld[RT_WORLD_ROW_CAMERA];
    const uint4 r3 = gRtWorld[RT_WORLD_ROW_SUN_WORLD];
    const uint4 r4 = gRtWorld[RT_WORLD_ROW_GRID];
    const uint4 r5 = gRtWorld[RT_WORLD_ROW_WINDOW];
    const uint4 r6 = gRtWorld[RT_WORLD_ROW_COUNTS];
    const uint4 r7 = gRtWorld[RT_WORLD_ROW_CALIBRATION];
    RtWorld w;
    w.serial = r0.x;
    w.flags = r0.y;
    w.hour = asfloat(r0.z);
    w.lampFactor = asfloat(r0.w);
    w.units = asfloat(r1.x);
    w.wet = asfloat(r1.y);
    w.weather = r1.z;
    w.time = asfloat(r1.w);
    w.camera = asfloat(r2.xyz);
    w.sunNight = asfloat(r2.w);
    w.toSunWorld = asfloat(r3.xyz);
    w.originX = asfloat(r4.x);
    w.originZ = asfloat(r4.y);
    w.cellM = asfloat(r4.z);
    w.lookupM = asfloat(r4.w);
    w.cx0 = int(asfloat(r5.x));
    w.cz0 = int(asfloat(r5.y));
    w.nx = r5.z;
    w.nz = r5.w;
    w.lampCount = r6.x;
    w.lampsInWindow = r6.y;
    w.cells = r6.z;
    w.liveCount = r6.w;
    w.statsFrame = r7.x;
    w.windows = r7.y;
    w.worldKind = r7.w;
    w.axisX = asfloat(gRtWorld[5u].xyz);
    w.axisY = asfloat(gRtWorld[6u].xyz);
    w.axisZ = asfloat(gRtWorld[7u].xyz);
    const uint4 r8 = gRtWorld[RT_WORLD_ROW_SCHEDULE];
    w.street = asfloat(r8.x);
    w.fill = asfloat(r8.y);
    w.liveShining = r8.z;
    w.evidence = r8.w;
    return w;
}

// The hit frame of the WORLD whatever RT_HIT_FRAME_ROW means in this pass (the car passes' own frame rows are the car's light; a city hit wants the world's): the same unpacking as RtHitLoadFrame.
RtHitFrame RtWorldHitFrame()
{
    RtHitFrame f;
    const uint4 r0 = gRtWorld[0u];
    const uint4 r1 = gRtWorld[1u];
    const uint4 r2 = gRtWorld[2u];
    const uint4 r3 = gRtWorld[3u];
    const uint4 r4 = gRtWorld[4u];
    f.toSun = asfloat(r0.xyz);
    f.flags = r0.w;
    f.sun = asfloat(r1.xyz);
    f.shadowReach = asfloat(r1.w);
    f.ambient = asfloat(r2.xyz);
    f.ambientGain = asfloat(r2.w);
    f.envSharp = r3.x;
    f.envBlur = r3.y;
    f.night = asfloat(r3.z);
    f.specGain = asfloat(r3.w);
    f.sky = asfloat(r4.xyz);
    f.gain = asfloat(r4.w);
    f.axisX = asfloat(gRtWorld[5u].xyz);
    f.axisY = asfloat(gRtWorld[6u].xyz);
    f.axisZ = asfloat(gRtWorld[7u].xyz);
    f.detail = asfloat(gRtWorld[5u].w);
    return f;
}

// The class's gain (rt_hit_calibration.h ClassGains::Table): the model's exposure against the picture, 1 until a window has moved it. CITY_HIT_GAIN(cls) is RtWorldClassGain(cls).
float RtWorldClassGain(uint cls)
{
    const uint c = min(cls, RT_WORLD_CLASSES - 1u);
    const uint4 r = gRtWorld[RT_WORLD_ROW_GAINS + (c >> 2u)];
    const uint k = c & 3u;
    return asfloat(k == 0u ? r.x : (k == 1u ? r.y : (k == 2u ? r.z : r.w)));
}

// What a glowing material of this kind glows by (CITY-RT R7; the library's CITY_HIT_GLOW_FACTOR(kind)): a lamp lens by the street lamps' schedule, a traffic signal always, everything else (neon, signs, lit
// windows, interiors) by the fill lights' night factor. Row 47 only: one load.
float RtWorldGlowFactor(uint kind)
{
    const uint4 r = gRtWorld[RT_WORLD_ROW_SCHEDULE];
    return kind == RT_WORLD_GLOW_ALWAYS ? 1.0 : asfloat(kind == RT_WORLD_GLOW_LENS ? r.x : r.y);
}

// A view space point in the game's world: the camera plus the point along the world's axes.
float3 RtWorldPoint(RtWorld w, float3 view)
{
    return w.camera + float3(dot(view, w.axisX), dot(view, w.axisY), dot(view, w.axisZ));
}

// ---- the lamps ------------------------------------------------------------------------------------------------------------------------------------

// Row k (0..2) of lamp `lamp` (the layout is city_hit_shade_rules.h MakeGpuLamp's: CityHitLampFromRows takes the three rows).
uint4 RtLampRow(RtWorld w, uint lamp, uint k)
{
    return gRtLamps[1u + (w.cells + 1u) / 2u + RT_WORLD_LAMP_ROWS * lamp + k];
}

struct RtLampCell
{
    uint first;
    uint count;
};

// The lamps of window cell (cx, cz) (window relative, inside the window).
RtLampCell RtLampCellAt(RtWorld w, int cx, int cz)
{
    const uint c = uint(cz) * w.nx + uint(cx);
    const uint4 row = gRtLamps[1u + c / 2u];
    RtLampCell r;
    r.first = (c & 1u) == 0u ? row.x : row.z;
    r.count = (c & 1u) == 0u ? row.y : row.w;
    return r;
}

// A walk over the lamps of the cells that cover a square around a point: `index` is the lamp now (RtLampRow), `done` 1 at the end. The order is the cells row by row (z outer, x inner), a cell's
// lamps in the buffer's order: the twin (rt_world_frame_rules.h LampBegin / LampStep) walks it the same way.
struct RtLampCursor
{
    int cx;       // the cell now (window relative)
    int cz;
    int x0;       // the first and last column of the walk, its last row
    int x1;
    int z1;
    uint index;
    uint end;     // where the lamps of the cell now end
    uint done;
};

// From the cell the cursor stands on forward to a cell with lamps (its first lamp), or to the end.
RtLampCursor RtLampSettle(RtWorld w, RtLampCursor c)
{
    RT_WORLD_LOOP
    for (uint guard = 0u; guard <= RT_WORLD_LOOKUP_CELLS; ++guard)
    {
        const RtLampCell cell = RtLampCellAt(w, c.cx, c.cz);
        if (cell.count != 0u)
        {
            c.index = cell.first;
            c.end = cell.first + cell.count;
            return c;
        }
        c.cx = c.cx + 1;
        if (c.cx > c.x1)
        {
            c.cx = c.x0;
            c.cz = c.cz + 1;
            if (c.cz > c.z1)
            {
                c.done = 1u;
                return c;
            }
        }
    }
    c.done = 1u;
    return c;
}

// The walk around the VIEW space point `view`, the radius in metres (0 .. two cell sides). Done at once where the window holds no lamp or the point is outside it.
RtLampCursor RtLampBegin(RtWorld w, float3 view, float radius)
{
    RtLampCursor c;
    c.cx = 0;
    c.cz = 0;
    c.x0 = 0;
    c.x1 = -1;
    c.z1 = -1;
    c.index = 0u;
    c.end = 0u;
    c.done = 1u;
    if ((w.flags & RT_WORLD_LAMPS) == 0u || w.nx == 0u || w.nz == 0u || w.lampCount == 0u || !(w.cellM > 0.0))
    {
        return c;
    }
    const float3 p = RtWorldPoint(w, view);
    const float r = clamp(radius, 0.0, RT_WORLD_LOOKUP_MAX_CELLS * w.cellM);
    int x0 = int(floor((p.x - r - w.originX) / w.cellM)) - w.cx0;
    int x1 = int(floor((p.x + r - w.originX) / w.cellM)) - w.cx0;
    int z0 = int(floor((p.z - r - w.originZ) / w.cellM)) - w.cz0;
    int z1 = int(floor((p.z + r - w.originZ) / w.cellM)) - w.cz0;
    x0 = max(x0, 0);
    z0 = max(z0, 0);
    x1 = min(x1, int(w.nx) - 1);
    z1 = min(z1, int(w.nz) - 1);
    x1 = min(x1, x0 + (RT_WORLD_LOOKUP_AXIS - 1));
    z1 = min(z1, z0 + (RT_WORLD_LOOKUP_AXIS - 1));
    if (x0 > x1 || z0 > z1)
    {
        return c;
    }
    c.cx = x0;
    c.cz = z0;
    c.x0 = x0;
    c.x1 = x1;
    c.z1 = z1;
    c.done = 0u;
    return RtLampSettle(w, c);
}

// The next lamp.
RtLampCursor RtLampStep(RtWorld w, RtLampCursor c)
{
    if (c.done != 0u)
    {
        return c;
    }
    c.index = c.index + 1u;
    if (c.index < c.end)
    {
        return c;
    }
    c.cx = c.cx + 1;
    if (c.cx > c.x1)
    {
        c.cx = c.x0;
        c.cz = c.cz + 1;
        if (c.cz > c.z1)
        {
            c.done = 1u;
            return c;
        }
    }
    return RtLampSettle(w, c);
}

#endif  // MCLA_RT_WORLD_FRAME_HLSLI
