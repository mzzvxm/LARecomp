// AI rubberband (mcRubberBandMgr): the off switch, a forced difficulty level,
// scales over the parsed RubberBandTune entries, and a dump of them.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

bool Patch_DisableRubberBanding() {
    return larecomp::settings::disable_rubberbanding.Get();
}

// ── mcRubberBandMgr tuning ──────────────────────────────────────────────────
//
// sub_827440E8 allocates the 1876-byte manager and parks it in dword_8290AC0C;
// sub_82743A10 (RubberBandMgr_LoadTuning) fills 11 mcRubberBandTuning entries at
// mgr+76, stride 140, from $/tune/career/RubberBandTune00..10 — then rescales a
// few of them in code (+48 *= 2, +56 *= 2.5, +64 *= 2), so the file value is not
// the final value. Field names and offsets come from the parser registration in
// sub_823990E0, same shape as fragTuneStruct.
//
// disable_rubberbanding kills the whole system, which also removes MinThrottle —
// the leash that slows the AI down when it is AHEAD. That is why turning it off
// makes races harder rather than fairer. Scaling the individual limits keeps
// both halves and just changes how hard they pull.
static constexpr uint32_t kRubberBandMgrPtr = 0x8290AC0Cu;
static constexpr uint32_t kRubberBandFirst = 76u;
static constexpr uint32_t kRubberBandStride = 140u;
static constexpr int kRubberBandCount = 11;

struct RubberBandField {
    uint32_t offset;
    const char* name;
};

// The scalar members. +76 AheadDistOffsets and +108 BehindDistOffsets are
// 8-float arrays and are dumped separately.
static constexpr RubberBandField kRubberBandFields[] = {
    {0,  "MaxThrottle"},
    {4,  "MinSpeed"},
    {8,  "MinDist"},
    {12, "MaxDist"},
    {16, "MinThrottle"},
    {20, "BehindMinDist"},
    {24, "BehindMaxDist"},
    {28, "BehindMaxThrottle"},
    {32, "OutsideTrafficBubbleThresholdSpeed"},
    {36, "OutsideTrafficBubbleThrottlePenalty"},
    {40, "DistBehindBeforeTakingShortcuts"},
    {44, "UseNitroMinDistFromStart"},
    {48, "UseNitroMinDistFromFinish"},
    {52, "UseNitroMinDistBehindPlayer"},
    {56, "UnlimitedNitroMinDistBehindPlayer"},
    {60, "HomeStretchBehindMaxThrottle"},
    {64, "HomeStretchDistance"},
    {68, "PlayerCloseMaxThrottle"},
    {72, "PlayerCloseDistance"},
};

// Writes every entry to <exe>/rubberband_dump.txt. The knobs below are meant to
// be chosen from these numbers rather than guessed at.
void DumpRubberBandTuning() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) {
        LARECOMP_APP_ERROR("[RubberBand] manager not created yet");
        return;
    }

    std::error_code ec;
    std::ofstream out(std::filesystem::current_path(ec) / "rubberband_dump.txt");
    if (!out) {
        LARECOMP_APP_ERROR("[RubberBand] cannot open rubberband_dump.txt");
        return;
    }
    out << "mcRubberBandMgr 0x" << std::hex << mgr << std::dec
        << "  (11 x mcRubberBandTuning, 140 bytes, at mgr+76)\n"
        << "values are post-LoadTuning: +48 *= 2, +56 *= 2.5, +64 *= 2 "
        << "(difficulty_preset = original puts the values as parsed back)\n\n";

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        out << "RubberBandTune" << (i < 10 ? "0" : "") << i << "  @0x" << std::hex << e
            << std::dec << "\n";
        for (const auto& f : kRubberBandFields) {
            char line[128];
            std::snprintf(line, sizeof(line), "  +%-3u %-36s %g\n", f.offset, f.name,
                          double(ReadGuestF32(base, e + f.offset)));
            out << line;
        }
        for (const char* which : {"AheadDistOffsets", "BehindDistOffsets"}) {
            const uint32_t off = (which[0] == 'A') ? 76u : 108u;
            out << "  +" << off << " " << which << " ";
            for (int k = 0; k < 8; ++k) out << ReadGuestF32(base, e + off + 4u * k) << " ";
            out << "\n";
        }
        out << "\n";
    }
    LARECOMP_APP_INFO("[RubberBand] dumped {} entries to rubberband_dump.txt", kRubberBandCount);
}

// Which fields each scale drives. `throttle` marks the normalised 0..1 fields,
// which are not pushed past 1.0 when scaling up.
struct RubberBandScale {
    const char* cvar;
    uint32_t offsets[4];  // 0-terminated
    bool throttle;
    // Scales multiply the loaded value and are neutral at 1.0. Absolute knobs
    // write the value straight in and are neutral at -1 — used where the shipped
    // tune is the same at every difficulty level, so a multiplier would be
    // meaningless to reason about.
    bool absolute;
};

static constexpr RubberBandScale kRubberBandScales[] = {
    {"rubberband_catchup_scale",      {0, 28, 60, 68}, true,  false},  // *MaxThrottle
    {"rubberband_holdback_scale",     {16, 0, 0, 0},   true,  false},  // MinThrottle
    {"rubberband_distance_scale",     {8, 12, 20, 24}, false, false},  // *Dist bands
    {"rubberband_shortcut_scale",     {40, 0, 0, 0},   false, false},  // DistBehindBeforeTakingShortcuts
    {"rubberband_nitro_start_dist",   {44, 0, 0, 0},   false, true},   // UseNitroMinDistFromStart
    {"rubberband_nitro_finish_scale", {48, 0, 0, 0},   false, false},  // UseNitroMinDistFromFinish
    {"rubberband_nitro_behind_scale", {52, 56, 0, 0},  false, false},  // *MinDistBehindPlayer
};

static bool RubberBandGroupNeutral(const RubberBandScale& g, double v) {
    return g.absolute ? (v < 0.0) : (v == 1.0);
}

// Offset 0 is a real field (MaxThrottle) and only ever appears first, so a zero
// past index 0 is the terminator.
static constexpr size_t RubberBandFieldTotal() {
    size_t n = 0;
    for (const auto& g : kRubberBandScales)
        for (size_t i = 0; i < 4; ++i)
            if (i == 0 || g.offsets[i] != 0) ++n;
    return n;
}

// Same shape as the fragTune override: while every scale is 1.0 the loaded
// values are re-read as the baseline, so the scales always multiply the tune the
// game actually parsed rather than compounding on themselves. Throttle fields
// are clamped back to their original ceiling when that ceiling was <= 1.0, since
// those read as normalised throttle and overshooting them is not something the
// shipped tune ever does.
// mgr+60 holds the index of the RubberBandTune entry in force. Writing it every
// frame beats both places the game sets it: sub_82743B30 computes it at race
// setup, and the tail of that function can overwrite it again from the stored
// value at dword_8286EC70+2616.
static constexpr uint32_t kRubberBandLevelField = 60u;

void ApplyRubberBandLevel() {
    int want = int(std::strtol(larecomp::cvar::GetFlagByName("rubberband_level").c_str(),
                               nullptr, 10));
    if (want < 0) {
        const double pct = std::strtod(larecomp::cvar::GetFlagByName("ai_difficulty").c_str(), nullptr);
        if (pct >= 0.0) want = int(pct / 10.0 + 0.5);
    }
    SetRubberBandLevelOption(want);
}

// Set when the difficulty preset has just rewritten the tune table: the scales
// take their baseline from the new table instead of the one they last saw.
static bool g_rubberband_rebase = false;

void ApplyRubberBandScales() {
    static constexpr size_t kFieldCount = RubberBandFieldTotal();
    static float baseline[kRubberBandCount][kFieldCount] = {};
    static bool have_base = false;
    static bool was_scaled = false;
    if (g_rubberband_rebase) {
        g_rubberband_rebase = false;
        have_base = false;
        was_scaled = false;
    }

    double scale[std::size(kRubberBandScales)];
    bool neutral = true;
    for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
        scale[g] = std::strtod(larecomp::cvar::GetFlagByName(kRubberBandScales[g].cvar).c_str(),
                               nullptr);
        if (!RubberBandGroupNeutral(kRubberBandScales[g], scale[g])) neutral = false;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;
    // Entry 0 is still zero until LoadTuning has run; do not snapshot that.
    if (ReadGuestF32(base, mgr + kRubberBandFirst + 12) == 0.0f) return;

    // Dump once, automatically, the first frame the tune is actually parsed —
    // the scales below are only meaningful against these numbers, and waiting
    // for someone to press a button means the file is usually missing when it
    // is needed. The rubberband_dump cvar still forces a fresh one on demand.
    static bool auto_dumped = false;
    if (!auto_dumped) {
        auto_dumped = true;
        DumpRubberBandTuning();
    }

    // Every field this touches, in the order the baseline stores them.
    auto for_each_field = [&](auto&& fn) {
        size_t slot = 0;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            for (size_t i = 0; i < 4; ++i) {
                if (i != 0 && grp.offsets[i] == 0) break;
                fn(slot++, grp.offsets[i], scale[g], grp.throttle, grp.absolute);
            }
        }
    };

    if (neutral) {
        if (was_scaled) {
            for (int i = 0; i < kRubberBandCount; ++i) {
                const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
                for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                    WriteGuestF32(base, e + off, baseline[i][slot]);
                });
            }
            was_scaled = false;
            LARECOMP_APP_INFO("[RubberBand] scales cleared, shipped tune restored");
            return;
        }
        // Keep the baseline tracking whatever LoadTuning last produced.
        for (int i = 0; i < kRubberBandCount; ++i) {
            const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
            for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                baseline[i][slot] = ReadGuestF32(base, e + off);
            });
        }
        have_base = true;
        return;
    }

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        for_each_field([&](size_t slot, uint32_t off, double v, bool is_throttle,
                           bool is_absolute) {
            if (!have_base) baseline[i][slot] = ReadGuestF32(base, e + off);
            const float orig = baseline[i][slot];
            if (is_absolute) {
                // Neutral for this group: leave the shipped value in place.
                if (v < 0.0) {
                    WriteGuestF32(base, e + off, orig);
                    return;
                }
                WriteGuestF32(base, e + off, float(v));
                return;
            }
            double out = double(orig) * v;
            // Throttles are normalised — measured 0.75..1.0 across the 11 levels
            // — so do not push them past 1.0 when scaling up.
            if (is_throttle && orig <= 1.0f && out > 1.0) out = 1.0;
            WriteGuestF32(base, e + off, float(out));
        });
    }
    have_base = true;

    if (!was_scaled) {
        std::string what;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            if (RubberBandGroupNeutral(grp, scale[g])) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %s%g", what.empty() ? "" : ", ",
                          grp.cvar + 11,  // skip "rubberband_"
                          grp.absolute ? "= " : "x", scale[g]);
            what += buf;
        }
        LARECOMP_APP_INFO("[RubberBand] {} on {} entries", what, kRubberBandCount);
    }
    was_scaled = true;
}
// Live. SetupRace (sub_82743B30 -> sub_82742D28) copies the chosen tune entry,
// 140 bytes, into every AI racer at racer+412 once per race, and the AI brain
// (sub_822BE670: racer+452 shortcuts, +468 nitro; the mgr's per-frame pass:
// +384 offsets) reads that copy every frame. So the table edits above only reach
// the next race. Doing the same copy every frame makes them reach the race in
// progress: session = dword_8286EBF8, +8 -> entries at +8 (stride 192), entry+8 =
// type (2 = AI), entry+12 = racer; count at (session+8)+3080. Same layout
// sub_82742D28 walks. The level is also written into mgr+60 here (the dev option
// only feeds the next SetupRace).
void ApplyRubberBandLive() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;
    // +53 is set at the end of SetupRace, +52 is the system's enable flag.
    if (base[mgr + 53] == 0 || base[mgr + 52] == 0) return;

    const int forced = int(std::strtol(larecomp::cvar::GetFlagByName("rubberband_level").c_str(),
                                       nullptr, 10));
    const double pct = forced >= 0 ? -1.0
        : std::strtod(larecomp::cvar::GetFlagByName("ai_difficulty").c_str(), nullptr);
    int want = forced;
    if (want < 0 && pct >= 0.0) want = int(std::fmin(pct, 100.0) / 10.0 + 0.5);
    if (want > 10) want = 10;
    if (want >= 0 && ReadGuestU32(base, mgr + kRubberBandLevelField) != uint32_t(want))
        WriteGuestU32(base, mgr + kRubberBandLevelField, uint32_t(want));
    const uint32_t level = ReadGuestU32(base, mgr + kRubberBandLevelField);
    if (level > 10) return;
    // Levels past the game's last: the per-level ramp of each scalar field,
    // (level 10 - level 0) / 10, carried on. 150% = 5 levels beyond, 200% = 10.
    const double beyond = pct > 100.0 ? (pct - 100.0) / 10.0 : 0.0;

    const uint32_t session = ReadGuestU32(base, 0x8286EBF8u);
    if (session == 0) return;
    const uint32_t s = session + 8;
    const int32_t count = int32_t(ReadGuestU32(base, s + 3080));
    if (count <= 0 || count > 16) return;

    const uint32_t src = mgr + kRubberBandFirst + level * kRubberBandStride;
    const uint32_t easy = mgr + kRubberBandFirst;
    const uint32_t hard = mgr + kRubberBandFirst + 10u * kRubberBandStride;
    for (int i = 0; i < count; ++i) {
        const uint32_t entry = s + 8 + uint32_t(i) * 192u;
        if (ReadGuestU32(base, entry + 8) != 2) continue;  // AI only
        const uint32_t racer = ReadGuestU32(base, entry + 12);
        if (racer < 0x10000u || racer >= 0xA0000000u) continue;
        for (uint32_t o = 0; o < kRubberBandStride; o += 4) {
            if (beyond > 0.0 && o < 76u) {
                // Scalar floats only; the two 8-float offset arrays stay level 10.
                const float v10 = ReadGuestF32(base, hard + o);
                const float v0 = ReadGuestF32(base, easy + o);
                double v = double(v10) + double(v10 - v0) / 10.0 * beyond;
                if (v < 0.0) v = 0.0;
                // Normalised throttles stay inside 0..1.
                const bool throttle = o == 0 || o == 16 || o == 28 || o == 60 || o == 68;
                if (throttle && v10 <= 1.0f && v > 1.0) v = 1.0;
                if (ReadGuestF32(base, racer + 412 + o) != float(v))
                    WriteGuestF32(base, racer + 412 + o, float(v));
                continue;
            }
            const uint32_t w = ReadGuestU32(base, src + o);
            if (ReadGuestU32(base, racer + 412 + o) != w)
                WriteGuestU32(base, racer + 412 + o, w);
        }
    }
}

static bool DifficultyPresetOriginal() {
    return larecomp::cvar::GetFlagByName("difficulty_preset") == "original";
}

namespace {

constexpr uint32_t kRubberBandTableBytes = kRubberBandCount * kRubberBandStride;

// The table both ways, captured while LoadTuning runs: `raw` entry by entry as
// the parser fills it (before the Complete Edition's rescale), `complete` once
// LoadTuning returns. `generation` counts the loads, so a reload is noticed.
struct CapturedTune {
    uint32_t mgr = 0;            // the manager LoadTuning is filling, while it runs
    uint32_t parsed = 0;         // bit i: entry i was captured raw
    uint8_t raw[kRubberBandTableBytes] = {};
    uint8_t complete[kRubberBandTableBytes] = {};
    uint32_t loaded_mgr = 0;     // the manager both copies belong to
    uint32_t generation = 0;
};
CapturedTune g_tune;

// Set when the game starts and whenever the preset turns to original: the next
// race setup lifts a base below 3 to 3.
bool g_lift_base_pending = true;

}  // namespace

REX_EXTERN(__imp__rex_sub_82743A10);  // mcRubberBandMgr::LoadTuning
REX_EXTERN(__imp__rex_sub_821C04B0);  // the tune parser: loads a named file into a struct
REX_EXTERN(__imp__rex_sub_82742F28);  // mcRubberBandMgr: the race-result step
REX_EXTERN(__imp__rex_sub_82743B30);  // RubberBandMgr_SetupRace

// LoadTuning: r3 = the manager. It parses RubberBandTune00..10 into mgr+76
// (stride 140) through sub_821C04B0, then rescales. The parser wrapper below
// copies each entry as it lands; the table as LoadTuning leaves it is the
// Complete Edition's.
extern "C" REX_FUNC(rex_sub_82743A10) {
    const uint32_t mgr = ctx.r3.u32;
    g_tune.mgr = mgr;
    g_tune.parsed = 0;
    __imp__rex_sub_82743A10(ctx, base);
    g_tune.mgr = 0;
    if (g_tune.parsed != (1u << kRubberBandCount) - 1u) {
        LARECOMP_APP_ERROR("[DifficultyPreset] RubberBandTune parsed only partly (mask {:03X}); "
                           "the original preset stays off for this load", g_tune.parsed);
        g_tune.loaded_mgr = 0;
        return;
    }
    std::memcpy(g_tune.complete, base + mgr + kRubberBandFirst, kRubberBandTableBytes);
    g_tune.loaded_mgr = mgr;
    ++g_tune.generation;
}

// The parser: r7 = the struct it fills. Only while LoadTuning runs, and only
// for a destination that is one of the manager's eleven entries.
extern "C" REX_FUNC(rex_sub_821C04B0) {
    const uint32_t dest = ctx.r7.u32;
    __imp__rex_sub_821C04B0(ctx, base);
    const uint32_t mgr = g_tune.mgr;
    if (mgr == 0 || dest < mgr + kRubberBandFirst) return;
    const uint32_t off = dest - (mgr + kRubberBandFirst);
    if (off >= kRubberBandTableBytes || off % kRubberBandStride != 0) return;
    std::memcpy(g_tune.raw + off, base + dest, kRubberBandStride);
    g_tune.parsed |= 1u << (off / kRubberBandStride);
}

// The race-result step: r3 = the manager, r4 = won (low byte), r5 = the race's
// tier. mgr+56 base level, +60 level in force, +64 points. Same code as
// sub_82742F28 with the ceilings and floors a parameter.
static void RaceResultStep(uint8_t* base, uint32_t mgr, bool won, int32_t tier,
                           const int32_t (&win_ceiling)[4], const int32_t (&loss_floor)[4]) {
    static constexpr int32_t kWinPoints[4] = {4, 6, 9, 14};
    static constexpr int32_t kLossPoints[4] = {-14, -9, -6, -4};
    const bool known = tier >= 0 && tier <= 3;
    int32_t points = int32_t(ReadGuestU32(base, mgr + 64));
    const int32_t level = int32_t(ReadGuestU32(base, mgr + 56));
    int32_t limit = 5;
    if (won) {
        if (known) limit = win_ceiling[tier];
        if (level < limit) points += known ? kWinPoints[tier] : 0;
    } else {
        if (known) limit = loss_floor[tier];
        if (level > limit) points += known ? kLossPoints[tier] : 0;
    }
    auto set_level = [&](int32_t v) {
        WriteGuestU32(base, mgr + 56, uint32_t(v));
        WriteGuestU32(base, mgr + 60, uint32_t(v));
    };
    if (points >= 18) {
        if (level >= limit) return;  // the points are not stored either
        WriteGuestU32(base, mgr + 64, uint32_t(points % 18 - 18));
        const int32_t up = level + 1;
        set_level(up < 0 ? 0 : (up <= limit ? up : limit));
        return;
    }
    if (points <= -18) {
        if (level <= limit) return;
        WriteGuestU32(base, mgr + 64, uint32_t(points % -18 + 18));
        const int32_t down = level - 1;
        set_level(down < limit ? limit : (down > 10 ? 10 : down));
        return;
    }
    WriteGuestU32(base, mgr + 64, uint32_t(points));
}

extern "C" REX_FUNC(rex_sub_82742F28) {
    if (!DifficultyPresetOriginal()) {
        __imp__rex_sub_82742F28(ctx, base);
        return;
    }
    static constexpr int32_t kLaunchWinCeiling[4] = {5, 6, 8, 10};
    static constexpr int32_t kLaunchLossFloor[4] = {0, 2, 4, 5};
    const uint32_t mgr = ctx.r3.u32;
    const int32_t before = int32_t(ReadGuestU32(base, mgr + 56));
    RaceResultStep(base, mgr, (ctx.r4.u32 & 0xFFu) != 0, int32_t(ctx.r5.u32),
                   kLaunchWinCeiling, kLaunchLossFloor);
    const int32_t after = int32_t(ReadGuestU32(base, mgr + 56));
    if (after != before)
        LARECOMP_APP_INFO("[DifficultyPreset] career difficulty {} -> {} (launch limits)", before,
                          after);
}

extern "C" REX_FUNC(rex_sub_82743B30) {
    if (g_lift_base_pending && DifficultyPresetOriginal()) {
        g_lift_base_pending = false;
        const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
        if (mgr != 0) {
            const int32_t level = int32_t(ReadGuestU32(base, mgr + 56));
            if (level < 3) {
                WriteGuestU32(base, mgr + 56, 3u);
                LARECOMP_APP_INFO("[DifficultyPreset] career difficulty {} -> 3, where the launch "
                                  "build starts", level);
            }
        }
    }
    __imp__rex_sub_82743B30(ctx, base);
}

// Per frame: puts the preset's table into the manager when the preset changes
// or the tune was loaded again. Only then, so the scales above can keep writing
// over it; they re-read their baseline from what this wrote.
void ApplyDifficultyPreset() {
    static int applied = -1;
    static uint32_t applied_generation = 0;
    static int last_want = -1;
    const int want = DifficultyPresetOriginal() ? 1 : 0;
    // Switched on while the game runs: the next race lifts the base again.
    if (want == 1 && last_want == 0) g_lift_base_pending = true;
    last_want = want;
    if (g_tune.loaded_mgr == 0) return;
    if (want == applied && g_tune.generation == applied_generation) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    if (ReadGuestU32(base, kRubberBandMgrPtr) != g_tune.loaded_mgr) return;
    std::memcpy(base + g_tune.loaded_mgr + kRubberBandFirst, want ? g_tune.raw : g_tune.complete,
                kRubberBandTableBytes);
    const bool first = applied < 0;
    applied = want;
    applied_generation = g_tune.generation;
    g_rubberband_rebase = true;
    if (want || !first)
        LARECOMP_APP_INFO("[DifficultyPreset] {} tune in the table",
                          want ? "launch build (as parsed)" : "Complete Edition");
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_DisableRubberBanding() { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
