// MCLA GPU renderer -- the probe field owner's shared HLSL (package G1 of perf/CITY-RT-M3.md; src/gpu/gi_field_rules.h is the C++ twin and says what everything is).
//
// What the owner's three passes share (hlsl/gi_field_reset_cs.hlsl clears the probes a window move exposed, hlsl/gi_field_weights_cs.hlsl turns the probes' state into the weight map,
// hlsl/gi_field_view_ps.hlsl draws the probes as spheres): the state word of a probe, the weight rule, the slot of a coordinate and the coordinate of a slot, the reset list, and the
// traversal that finds the nearest probe sphere along a ray. src/gpu/gi_field_rules_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL types, as gi_frame_test.cpp does for
// gi_common.hlsli) and checks every function against the twin on random inputs, the traversal against a brute force over the probes, and every #define against gi_field_rules.h; that is
// why the text keeps to a subset both languages read: scalar arithmetic, small structs, no out parameters, no swizzles of more than one component, no attributes, and only the
// intrinsics abs, sqrt, min, max, saturate, floor, asfloat, asint.
//
// THE RESOURCES. The includer defines GIF_RESET_WORD(i) before including (word i of the reset list: a ByteAddressBuffer's Load(i * 4)). Nothing else is read here: the frame's rows
// (gi_common.hlsli, gi_frame.h) are the includer's own business.

#ifndef MCLA_GI_FIELD_COMMON_HLSLI
#define MCLA_GI_FIELD_COMMON_HLSLI

#ifndef GIF_RESET_WORD
#error "define GIF_RESET_WORD(i) before including gi_field_common.hlsli"
#endif

// ---- the numbers (src/gpu/gi_field_rules.h) ---------------------------------------------------------------------------------------------------

#define GIF_FORMAT_VERSION 1

#define GIF_STATE_UPDATES_MASK 255u
#define GIF_STATE_UPDATES_MAX 255u
#define GIF_STATE_INSIDE 256u
#define GIF_STATE_NEW 512u
#define GIF_STATE_BACK_SHIFT 10u
#define GIF_STATE_BACK_MASK 261120u
#define GIF_STATE_AFTER_RESET 512u
#define GIF_WEIGHT_RAMP_UPDATES 2.0f

#define GIF_RESET_HEADER_WORDS 4u
#define GIF_RESET_ENTRY_WORDS 8u
#define GIF_RESET_MAX_ENTRIES 12u
#define GIF_RESET_GROUPS_X 256u

#define GIF_SPHERE_RADIUS_SPACINGS 0.16f

// ---- the state word and the weight ------------------------------------------------------------------------------------------------------------

uint GifStateUpdates(uint state)
{
    return state & GIF_STATE_UPDATES_MASK;
}

bool GifStateInside(uint state)
{
    return (state & GIF_STATE_INSIDE) != 0u;
}

bool GifStateNew(uint state)
{
    return (state & GIF_STATE_NEW) != 0u;
}

uint GifStateBack(uint state)
{
    return (state & GIF_STATE_BACK_MASK) >> GIF_STATE_BACK_SHIFT;
}

uint GifMakeState(uint updates, bool inside, bool isNew, uint back)
{
    return min(updates, GIF_STATE_UPDATES_MAX) | (inside ? GIF_STATE_INSIDE : 0u) | (isNew ? GIF_STATE_NEW : 0u) | (min(back, 255u) << GIF_STATE_BACK_SHIFT);
}

// One more update blended: the count rises (saturating), the new bit goes, the rest is kept (the probe update, G2, writes this word back).
uint GifStateAfterUpdate(uint state)
{
    uint updates = GifStateUpdates(state);
    return (state & ~(GIF_STATE_UPDATES_MASK | GIF_STATE_NEW)) | (updates < GIF_STATE_UPDATES_MAX ? updates + 1u : GIF_STATE_UPDATES_MAX);
}

// The weight map's value of a probe (0 inside geometry, outside the safe range, never traced; a ramp over the first updates).
float GifProbeWeight(uint state, bool inRange)
{
    if (!inRange || GifStateInside(state))
    {
        return 0.0f;
    }
    return saturate((float)GifStateUpdates(state) / GIF_WEIGHT_RAMP_UPDATES);
}

// ---- slots and coordinates ----------------------------------------------------------------------------------------------------------------------

struct GifCoord
{
    int x;
    int y;
    int z;
};

uint GifWrapC(int c, uint dim)
{
    int d = (int)dim;
    int r = c % d;
    if (r < 0)
    {
        r = r + d;
    }
    return (uint)r;
}

// The one coordinate of the window [origin, origin + dims) stored at slot (sx, sy, sz).
GifCoord GifCoordOfSlot(int ox, int oy, int oz, uint dx, uint dy, uint dz, uint sx, uint sy, uint sz)
{
    GifCoord c;
    c.x = ox + (int)GifWrapC((int)sx - ox, dx);
    c.y = oy + (int)GifWrapC((int)sy - oy, dy);
    c.z = oz + (int)GifWrapC((int)sz - oz, dz);
    return c;
}

// ---- the reset list -------------------------------------------------------------------------------------------------------------------------------

struct GifResetProbe
{
    bool valid;
    uint cascade;
    int x;
    int y;
    int z;
};

// Which probe group g of the reset list is (gi_field_rules.h ResetProbeOf).
GifResetProbe GifResetProbeOf(uint g)
{
    GifResetProbe p;
    p.valid = false;
    p.cascade = 0u;
    p.x = 0;
    p.y = 0;
    p.z = 0;
    uint entries = min(GIF_RESET_WORD(0u), GIF_RESET_MAX_ENTRIES);
    uint total = GIF_RESET_WORD(1u);
    if (g >= total)
    {
        return p;
    }
    for (uint e = 0u; e < entries; ++e)
    {
        uint w = GIF_RESET_HEADER_WORDS + e * GIF_RESET_ENTRY_WORDS;
        uint first = GIF_RESET_WORD(w + 7u);
        uint ex = GIF_RESET_WORD(w + 4u);
        uint ey = GIF_RESET_WORD(w + 5u);
        uint ez = GIF_RESET_WORD(w + 6u);
        if (g >= first && g - first < ex * ey * ez)
        {
            uint local = g - first;
            p.valid = true;
            p.cascade = GIF_RESET_WORD(w + 0u);
            p.x = asint(GIF_RESET_WORD(w + 1u)) + (int)(local % ex);
            p.y = asint(GIF_RESET_WORD(w + 2u)) + (int)((local / ex) % ey);
            p.z = asint(GIF_RESET_WORD(w + 3u)) + (int)(local / (ex * ey));
            return p;
        }
    }
    return p;
}

// ---- the first probe sphere along a ray (the debug view) -------------------------------------------------------------------------------------

struct GifAxis
{
    float tMax;
    float tDelta;
    int step;
};

// The traversal of one axis through cells one unit wide: `uo` the ray's position at t = 0 in cell units, `ud` its direction in cell units per metre, `cell` the cell it starts in.
GifAxis GifAxisSetup(float uo, float ud, int cell)
{
    GifAxis a;
    if (ud > 0.0f)
    {
        a.step = 1;
        a.tDelta = 1.0f / ud;
        a.tMax = ((float)(cell + 1) - uo) / ud;
    }
    else if (ud < 0.0f)
    {
        a.step = -1;
        a.tDelta = -1.0f / ud;
        a.tMax = ((float)cell - uo) / ud;
    }
    else
    {
        a.step = 0;
        a.tDelta = 1.0e30f;
        a.tMax = 1.0e30f;
    }
    return a;
}

struct GifProbeHit
{
    bool hit;
    float t;  // metres along the ray (the direction is unit)
    int cx;   // the probe's lattice coordinate
    int cy;
    int cz;
};

// The nearest probe sphere along the ray o + t d (d unit, 0 < t < tLimit) among the probes of the window [(gox, goy, goz), + (gdx, gdy, gdz)) of a lattice of `spacing` metres (the probe
// with coordinate c is at c x spacing) with spheres of `radius` (under half a spacing: each sphere is inside its own cell, which has the probe at its centre, so the first sphere met
// walking the cells along the ray is the nearest). The cells are walked as a 3D grid (Amanatides and Woo), one sphere test a cell.
GifProbeHit GifTraceProbes(float ox, float oy, float oz, float dx, float dy, float dz, float tLimit, int gox, int goy, int goz, uint gdx, uint gdy, uint gdz, float spacing,
                           float radius)
{
    GifProbeHit h;
    h.hit = false;
    h.t = 0.0f;
    h.cx = 0;
    h.cy = 0;
    h.cz = 0;
    float inv = 1.0f / spacing;
    // The ray in cell units: cell i spans [i - 0.5, i + 0.5] spacings round the probe, so u = p / spacing + 0.5 and the cell is floor(u).
    float uox = ox * inv + 0.5f;
    float uoy = oy * inv + 0.5f;
    float uoz = oz * inv + 0.5f;
    float udx = dx * inv;
    float udy = dy * inv;
    float udz = dz * inv;
    // The part of the ray inside the window's box.
    float tmin = 0.0f;
    float tmax = tLimit;
    float lox = (float)gox;
    float loy = (float)goy;
    float loz = (float)goz;
    float hix = (float)(gox + (int)gdx);
    float hiy = (float)(goy + (int)gdy);
    float hiz = (float)(goz + (int)gdz);
    if (udx == 0.0f)
    {
        if (uox < lox || uox >= hix)
        {
            return h;
        }
    }
    else
    {
        float t0 = (lox - uox) / udx;
        float t1 = (hix - uox) / udx;
        tmin = max(tmin, min(t0, t1));
        tmax = min(tmax, max(t0, t1));
    }
    if (udy == 0.0f)
    {
        if (uoy < loy || uoy >= hiy)
        {
            return h;
        }
    }
    else
    {
        float t0 = (loy - uoy) / udy;
        float t1 = (hiy - uoy) / udy;
        tmin = max(tmin, min(t0, t1));
        tmax = min(tmax, max(t0, t1));
    }
    if (udz == 0.0f)
    {
        if (uoz < loz || uoz >= hiz)
        {
            return h;
        }
    }
    else
    {
        float t0 = (loz - uoz) / udz;
        float t1 = (hiz - uoz) / udz;
        tmin = max(tmin, min(t0, t1));
        tmax = min(tmax, max(t0, t1));
    }
    if (!(tmin <= tmax))
    {
        return h;
    }
    // The first cell: where the ray is at tmin, kept inside the window (the entry point lies on a boundary: the cell it is in is either side of it, rounding included).
    int cx = min(max((int)floor(uox + udx * tmin), gox), gox + (int)gdx - 1);
    int cy = min(max((int)floor(uoy + udy * tmin), goy), goy + (int)gdy - 1);
    int cz = min(max((int)floor(uoz + udz * tmin), goz), goz + (int)gdz - 1);
    GifAxis ax = GifAxisSetup(uox, udx, cx);
    GifAxis ay = GifAxisSetup(uoy, udy, cy);
    GifAxis az = GifAxisSetup(uoz, udz, cz);
    uint steps = gdx + gdy + gdz + 3u;
    for (uint i = 0u; i < steps; ++i)
    {
        // the sphere of this cell's probe
        float ocx = ox - (float)cx * spacing;
        float ocy = oy - (float)cy * spacing;
        float ocz = oz - (float)cz * spacing;
        float b = ocx * dx + ocy * dy + ocz * dz;
        float c = ocx * ocx + ocy * ocy + ocz * ocz - radius * radius;
        float disc = b * b - c;
        if (disc >= 0.0f)
        {
            float t = -b - sqrt(disc);
            if (t > 0.0f && t < tLimit)
            {
                h.hit = true;
                h.t = t;
                h.cx = cx;
                h.cy = cy;
                h.cz = cz;
                return h;
            }
        }
        // the next cell: across the nearest boundary
        float tn = min(ax.tMax, min(ay.tMax, az.tMax));
        if (tn > tmax)
        {
            return h;
        }
        if (ax.tMax <= ay.tMax && ax.tMax <= az.tMax)
        {
            cx = cx + ax.step;
            ax.tMax = ax.tMax + ax.tDelta;
        }
        else if (ay.tMax <= az.tMax)
        {
            cy = cy + ay.step;
            ay.tMax = ay.tMax + ay.tDelta;
        }
        else
        {
            cz = cz + az.step;
            az.tMax = az.tMax + az.tDelta;
        }
        if (cx < gox || cx >= gox + (int)gdx || cy < goy || cy >= goy + (int)gdy || cz < goz || cz >= goz + (int)gdz)
        {
            return h;
        }
    }
    return h;
}

#endif
